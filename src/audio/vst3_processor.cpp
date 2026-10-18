#include "vst3_processor.hpp"

#include <utility>

#if defined(VST_HOST_ENABLE_VST3)

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

namespace Steinberg {
FUnknown* gStandardPluginContext = new Vst::HostApplication();
}

#endif

namespace vst_host::audio {

struct Vst3Processor::Impl {
    explicit Impl(std::string path) : path(std::move(path)) {}

    std::string path;
    bool loaded{false};
    AudioProcessSpec spec{};
    std::uint32_t latency{0};

#if defined(VST_HOST_ENABLE_VST3)
    std::shared_ptr<VST3::Hosting::Module> module;
    Steinberg::IPtr<Steinberg::Vst::PlugProvider> provider;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;
    Steinberg::Vst::AudioBusBuffers input_bus{};
    Steinberg::Vst::AudioBusBuffers output_bus{};
    Steinberg::Vst::AudioProcessData process_data{};
    Steinberg::Vst::ProcessContext process_context{};
    float* input_channels[2]{};
    float* output_channels[2]{};
    std::vector<float> input_left;
    std::vector<float> input_right;
    std::vector<float> output_left;
    std::vector<float> output_right;
    std::atomic<bool> process_failed{false};
    std::int64_t project_time_samples{0};
#endif
};

Vst3Processor::Vst3Processor(std::string plugin_path)
    : impl_(std::make_unique<Impl>(std::move(plugin_path))) {}

Vst3Processor::~Vst3Processor() {
#if defined(VST_HOST_ENABLE_VST3)
    if (impl_ && impl_->processor) {
        impl_->processor->setProcessing(false);
    }
    if (impl_ && impl_->component) {
        impl_->component->setActive(false);
    }
#endif
}

bool Vst3Processor::load(std::string& error) {
#if !defined(VST_HOST_ENABLE_VST3)
    error = "VST3 support is disabled. Configure with -DVST3_SDK_ROOT=/path/to/vst3sdk.";
    return false;
#else
    if (impl_->loaded) {
        return true;
    }

    impl_->module = VST3::Hosting::Module::create(impl_->path, error);
    if (!impl_->module) {
        error = "Could not load VST3 module '" + impl_->path + "': " + error;
        return false;
    }

    auto factory = impl_->module->getFactory();
    const auto classes = factory.classInfos();
    const auto selected = std::find_if(classes.begin(), classes.end(), [](const auto& info) {
        return info.category() == Steinberg::Vst::kVstAudioEffectClass;
    });
    if (selected == classes.end()) {
        error = "The VST3 module contains no audio-effect class: " + impl_->path;
        impl_->module.reset();
        return false;
    }

    impl_->provider = Steinberg::owned(
        new Steinberg::Vst::PlugProvider(factory, *selected, true));
    if (!impl_->provider || !impl_->provider->initialize()) {
        error = "Could not initialize the VST3 plug-in: " + impl_->path;
        impl_->provider = nullptr;
        impl_->module.reset();
        return false;
    }

    impl_->component = Steinberg::owned(impl_->provider->getComponent());
    if (!impl_->component) {
        error = "The VST3 plug-in did not provide an audio component.";
        impl_->provider = nullptr;
        impl_->module.reset();
        return false;
    }

    Steinberg::Vst::IAudioProcessor* raw_processor = nullptr;
    const auto query_result = impl_->component->queryInterface(
        Steinberg::Vst::IAudioProcessor::iid,
        reinterpret_cast<void**>(&raw_processor));
    if (query_result != Steinberg::kResultOk || raw_processor == nullptr) {
        error = "The VST3 audio component does not implement IAudioProcessor.";
        impl_->component = nullptr;
        impl_->provider = nullptr;
        impl_->module.reset();
        return false;
    }
    impl_->processor = raw_processor;
    raw_processor->release();

    impl_->loaded = true;
    return true;
#endif
}

bool Vst3Processor::prepare(const AudioProcessSpec& spec) {
    if (spec.sample_rate == 0 || spec.channels != 2 || spec.max_block_size == 0) {
        return false;
    }
#if !defined(VST_HOST_ENABLE_VST3)
    (void)spec;
    return false;
#else
    if (!impl_->loaded || !impl_->processor || !impl_->component) {
        return false;
    }

    impl_->spec = spec;
    const auto block_size = static_cast<std::size_t>(spec.max_block_size);
    impl_->input_left.assign(block_size, 0.0F);
    impl_->input_right.assign(block_size, 0.0F);
    impl_->output_left.assign(block_size, 0.0F);
    impl_->output_right.assign(block_size, 0.0F);
    impl_->input_channels[0] = impl_->input_left.data();
    impl_->input_channels[1] = impl_->input_right.data();
    impl_->output_channels[0] = impl_->output_left.data();
    impl_->output_channels[1] = impl_->output_right.data();

    const auto input_count = impl_->component->getBusCount(Steinberg::Vst::kAudio,
                                                           Steinberg::Vst::kInput);
    const auto output_count = impl_->component->getBusCount(Steinberg::Vst::kAudio,
                                                            Steinberg::Vst::kOutput);
    if (input_count < 1 || output_count < 1) {
        return false;
    }

    // This first implementation deliberately supports the common stereo
    // main-input/main-output effect topology. Other buses remain inactive.
    for (Steinberg::int32 i = 0; i < input_count; ++i) {
        if (impl_->component->activateBus(Steinberg::Vst::kAudio,
                Steinberg::Vst::kInput, i, i == 0) != Steinberg::kResultOk) {
            return false;
        }
    }
    for (Steinberg::int32 i = 0; i < output_count; ++i) {
        if (impl_->component->activateBus(Steinberg::Vst::kAudio,
                Steinberg::Vst::kOutput, i, i == 0) != Steinberg::kResultOk) {
            return false;
        }
    }

    Steinberg::Vst::SpeakerArrangement input_arrangement =
        Steinberg::Vst::SpeakerArr::kStereo;
    Steinberg::Vst::SpeakerArrangement output_arrangement =
        Steinberg::Vst::SpeakerArr::kStereo;
    if (impl_->processor->setBusArrangements(&input_arrangement, 1,
            &output_arrangement, 1) != Steinberg::kResultOk) {
        return false;
    }

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(spec.max_block_size);
    setup.sampleRate = static_cast<double>(spec.sample_rate);
    if (impl_->processor->setupProcessing(setup) != Steinberg::kResultOk) {
        return false;
    }

    impl_->process_context = {};
    impl_->process_context.sampleRate = static_cast<double>(spec.sample_rate);
    impl_->process_context.state = Steinberg::Vst::ProcessContext::kPlaying;

    impl_->input_bus = {};
    impl_->input_bus.numChannels = 2;
    impl_->input_bus.channelBuffers32 = impl_->input_channels;
    impl_->output_bus = {};
    impl_->output_bus.numChannels = 2;
    impl_->output_bus.channelBuffers32 = impl_->output_channels;

    impl_->process_data = {};
    impl_->process_data.processMode = Steinberg::Vst::kRealtime;
    impl_->process_data.symbolicSampleSize = Steinberg::Vst::kSample32;
    impl_->process_data.numInputs = 1;
    impl_->process_data.numOutputs = 1;
    impl_->process_data.inputs = &impl_->input_bus;
    impl_->process_data.outputs = &impl_->output_bus;
    impl_->process_data.processContext = &impl_->process_context;

    if (impl_->component->setActive(true) != Steinberg::kResultOk) {
        return false;
    }
    if (impl_->processor->setProcessing(true) != Steinberg::kResultOk) {
        impl_->component->setActive(false);
        return false;
    }
    impl_->latency = impl_->processor->getLatencySamples();
    impl_->project_time_samples = 0;
    return true;
#endif
}

void Vst3Processor::process(float* samples, std::size_t frames) noexcept {
    if (samples == nullptr || frames == 0) {
        return;
    }
#if defined(VST_HOST_ENABLE_VST3)
    if (!impl_->loaded || !impl_->processor || impl_->input_left.empty()) {
        return;
    }

    std::size_t offset = 0;
    const auto capacity = impl_->input_left.size();
    while (offset < frames) {
        const auto block_frames = std::min(capacity, frames - offset);
        for (std::size_t i = 0; i < block_frames; ++i) {
            impl_->input_left[i] = samples[(offset + i) * 2];
            impl_->input_right[i] = samples[(offset + i) * 2 + 1];
            // If a plug-in reports a processing failure, preserve dry audio.
            impl_->output_left[i] = impl_->input_left[i];
            impl_->output_right[i] = impl_->input_right[i];
        }

        impl_->process_data.numSamples = static_cast<Steinberg::int32>(block_frames);
        impl_->process_context.projectTimeSamples = impl_->project_time_samples;
        const auto result = impl_->processor->process(impl_->process_data);
        if (result != Steinberg::kResultOk) {
            impl_->process_failed.store(true, std::memory_order_relaxed);
            std::memcpy(impl_->output_left.data(), impl_->input_left.data(),
                        block_frames * sizeof(float));
            std::memcpy(impl_->output_right.data(), impl_->input_right.data(),
                        block_frames * sizeof(float));
        }

        for (std::size_t i = 0; i < block_frames; ++i) {
            samples[(offset + i) * 2] = impl_->output_left[i];
            samples[(offset + i) * 2 + 1] = impl_->output_right[i];
        }
        offset += block_frames;
        impl_->project_time_samples += static_cast<std::int64_t>(block_frames);
    }
#else
    (void)frames;
#endif
}

void Vst3Processor::reset() noexcept {
#if defined(VST_HOST_ENABLE_VST3)
    impl_->project_time_samples = 0;
    impl_->process_failed.store(false, std::memory_order_relaxed);
#endif
}

std::uint32_t Vst3Processor::latency_frames() const noexcept {
    return impl_->latency;
}

const std::string& Vst3Processor::plugin_path() const noexcept {
    return impl_->path;
}

} // namespace vst_host::audio
