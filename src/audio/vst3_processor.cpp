#include "vst3_processor.hpp"

#include <utility>
#include <cstdio>

#if defined(VST_HOST_ENABLE_VST3)

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

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
    Steinberg::Vst::ProcessData process_data{};
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
    error = "VST3 support is disabled. Configure with -DVST_HOST_ENABLE_VST3=ON.";
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
        return info.category() == kVstAudioEffectClass;
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
    impl_->processor = Steinberg::owned(raw_processor);

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
        std::fprintf(stderr,
                     "VST3: plugin has insufficient audio buses (inputs=%d, outputs=%d).\n",
                     input_count, output_count);
        return false;
    }

    // This first implementation deliberately supports the common stereo
    // main-input/main-output effect topology. Other buses remain inactive.
    for (Steinberg::int32 i = 0; i < input_count; ++i) {
        const auto result = impl_->component->activateBus(
            Steinberg::Vst::kAudio, Steinberg::Vst::kInput, i, i == 0);
        if (result != Steinberg::kResultOk) {
            std::fprintf(stderr, "VST3: activateBus(input, %d, %s) failed (result=%d).\n",
                         i, i == 0 ? "active" : "inactive", static_cast<int>(result));
            return false;
        }
    }
    for (Steinberg::int32 i = 0; i < output_count; ++i) {
        const auto result = impl_->component->activateBus(
            Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, i, i == 0);
        if (result != Steinberg::kResultOk) {
            std::fprintf(stderr, "VST3: activateBus(output, %d, %s) failed (result=%d).\n",
                         i, i == 0 ? "active" : "inactive", static_cast<int>(result));
            return false;
        }
    }

    // VST3 requires one arrangement entry for every audio bus, including
    // auxiliary/sidechain buses. Preserve the current layout of auxiliary
    // buses and request stereo for the main buses used by this host.
    std::vector<Steinberg::Vst::SpeakerArrangement> input_arrangements(
        static_cast<std::size_t>(input_count));
    std::vector<Steinberg::Vst::SpeakerArrangement> output_arrangements(
        static_cast<std::size_t>(output_count));

    for (Steinberg::int32 i = 0; i < input_count; ++i) {
        auto& arrangement = input_arrangements[static_cast<std::size_t>(i)];
        const auto result = impl_->processor->getBusArrangement(
            Steinberg::Vst::kInput, i, arrangement);
        if (result != Steinberg::kResultOk) {
            std::fprintf(stderr,
                         "VST3: getBusArrangement(input, %d) failed (result=%d).\\n",
                         i, static_cast<int>(result));
            return false;
        }
    }
    for (Steinberg::int32 i = 0; i < output_count; ++i) {
        auto& arrangement = output_arrangements[static_cast<std::size_t>(i)];
        const auto result = impl_->processor->getBusArrangement(
            Steinberg::Vst::kOutput, i, arrangement);
        if (result != Steinberg::kResultOk) {
            std::fprintf(stderr,
                         "VST3: getBusArrangement(output, %d) failed (result=%d).\\n",
                         i, static_cast<int>(result));
            return false;
        }
    }

    input_arrangements[0] = Steinberg::Vst::SpeakerArr::kStereo;
    output_arrangements[0] = Steinberg::Vst::SpeakerArr::kStereo;
    const auto arrangement_result = impl_->processor->setBusArrangements(
        input_arrangements.data(), input_count,
        output_arrangements.data(), output_count);
    if (arrangement_result != Steinberg::kResultOk) {
        std::fprintf(stderr,
                     "VST3: plugin rejected stereo main input/output bus arrangement "
                     "(inputs=%d, outputs=%d, result=%d).\\n",
                     input_count, output_count, static_cast<int>(arrangement_result));
        return false;
    }

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(spec.max_block_size);
    setup.sampleRate = static_cast<double>(spec.sample_rate);
    const auto setup_result = impl_->processor->setupProcessing(setup);
    if (setup_result != Steinberg::kResultOk) {
        std::fprintf(stderr,
                     "VST3: setupProcessing failed (rate=%.0f Hz, max block=%d, result=%d).\n",
                     setup.sampleRate, setup.maxSamplesPerBlock,
                     static_cast<int>(setup_result));
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

    const auto active_result = impl_->component->setActive(true);
    if (active_result != Steinberg::kResultOk) {
        std::fprintf(stderr, "VST3: component setActive(true) failed (result=%d).\n",
                     static_cast<int>(active_result));
        return false;
    }
    const auto processing_result = impl_->processor->setProcessing(true);
    if (processing_result != Steinberg::kResultOk &&
        processing_result != Steinberg::kNotImplemented) {
        std::fprintf(stderr, "VST3: processor setProcessing(true) failed (result=%d).\n",
                     static_cast<int>(processing_result));
        impl_->component->setActive(false);
        return false;
    }
    // Some plug-ins inherit the SDK's default implementation, which returns
    // kNotImplemented. Treat that as an unsupported optional transition rather
    // than rejecting the plug-in; process() remains the definitive test.
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
            bool expected = false;
            if (impl_->process_failed.compare_exchange_strong(
                    expected, true, std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "VST3: processor process() failed (result=%d); passing dry audio through.\n",
                             static_cast<int>(result));
            }
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
