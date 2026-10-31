#include "vst3_processor.hpp"

#include <utility>
#include <cstdio>

#if defined(VST_HOST_ENABLE_VST3)

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>
#include <cstdint>

namespace Steinberg {
FUnknown* gStandardPluginContext = new Vst::HostApplication();
}

namespace {

class EditorComponentHandler final : public Steinberg::Vst::IComponentHandler {
public:
    explicit EditorComponentHandler(Steinberg::Vst::IEditController* controller)
        : controller_(controller) {}

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                               Steinberg::Vst::ParamValue value) override {
        return controller_ ? controller_->setParamNormalized(id, value) : Steinberg::kResultFalse;
    }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32) override {
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override {
        if (obj == nullptr) return Steinberg::kInvalidArgument;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = this;
            addRef();
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    Steinberg::Vst::IEditController* controller_{};
};

class EditorFrame final : public Steinberg::IPlugFrame {
public:
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView*,
                                              Steinberg::ViewRect*) override {
        return Steinberg::kNotImplemented;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                  void** obj) override {
        if (obj == nullptr) return Steinberg::kInvalidArgument;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IPlugFrame::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = this;
            addRef();
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
};

} // namespace

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
    Steinberg::IPtr<Steinberg::Vst::IEditController> editor_controller;
    Steinberg::IPtr<Steinberg::IPlugView> editor_view;
    std::unique_ptr<EditorComponentHandler> editor_handler;
    std::unique_ptr<EditorFrame> editor_frame;
    Steinberg::Vst::AudioBusBuffers input_bus{};
    Steinberg::Vst::AudioBusBuffers output_bus{};
    Steinberg::Vst::ProcessData process_data{};
    Steinberg::Vst::ProcessContext process_context{};
    float* input_channels[2]{};
    float* output_channels[2]{};
    Steinberg::int32 input_channel_count{2};
    Steinberg::int32 output_channel_count{2};
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
    close_editor();
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

    // PlugProvider obtains the host context from PluginContextFactory. Creating a
    // HostApplication object alone is not enough: register it before initializing
    // the plug-in, otherwise JUCE VST3 controllers may initialize without a host
    // context and fail to bind to their component/editor processor.
    Steinberg::Vst::PluginContextFactory::instance().setPluginContext(
        Steinberg::gStandardPluginContext);

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


bool Vst3Processor::open_editor(void* native_parent, int& width, int& height,
                                std::string& error) {
#if !defined(VST_HOST_ENABLE_VST3)
    (void)native_parent;
    (void)width;
    (void)height;
    error = "VST3 support is disabled. Configure with -DVST_HOST_ENABLE_VST3=ON.";
    return false;
#else
    if (native_parent == nullptr || !impl_->provider) {
        error = "The VST3 plug-in is not loaded or the editor parent window is invalid.";
        std::fprintf(stderr,
                     "[VST3 editor] Cannot start editor creation (parent=%p, provider=%s).\n",
                     native_parent, impl_->provider ? "available" : "missing");
        return false;
    }
    std::fprintf(stderr,
                 "[VST3 editor] Starting editor creation for '%s' (X11 parent=0x%llx).\n",
                 impl_->path.c_str(),
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(native_parent)));
    if (impl_->editor_view) {
        Steinberg::ViewRect current{};
        if (impl_->editor_view->getSize(&current) == Steinberg::kResultTrue) {
            width = current.right - current.left;
            height = current.bottom - current.top;
        }
        return true;
    }

    impl_->editor_controller = Steinberg::owned(impl_->provider->getController());
    if (!impl_->editor_controller) {
        error = "The VST3 plug-in does not provide an edit controller.";
        return false;
    }
    impl_->editor_handler = std::make_unique<EditorComponentHandler>(impl_->editor_controller.get());
    impl_->editor_controller->setComponentHandler(impl_->editor_handler.get());
    std::fprintf(stderr, "[VST3 editor] Edit controller acquired; parameter count=%d.\n",
                 impl_->editor_controller->getParameterCount());

    // A separate VST3 edit controller must be synchronized with the component
    // state before its editor is created. JUCE-based plug-ins can otherwise
    // expose no parameters and return nullptr from createView().
    Steinberg::MemoryStream component_state;
    const auto state_result = impl_->component->getState(&component_state);
    if (state_result != Steinberg::kResultTrue) {
        std::fprintf(stderr, "[VST3 editor] component getState failed (result=%d).\n",
                     static_cast<int>(state_result));
        error = "Could not read the VST3 component state before creating its editor.";
        impl_->editor_controller->setComponentHandler(nullptr);
        impl_->editor_handler.reset();
        impl_->editor_controller = nullptr;
        return false;
    }

    const auto seek_result = component_state.seek(
        0, Steinberg::IBStream::kIBSeekSet, nullptr);
    if (seek_result != Steinberg::kResultOk) {
        std::fprintf(stderr, "[VST3 editor] component-state rewind failed (result=%d).\n",
                     static_cast<int>(seek_result));
        error = "Could not rewind the VST3 component state before synchronizing its editor.";
        impl_->editor_controller->setComponentHandler(nullptr);
        impl_->editor_handler.reset();
        impl_->editor_controller = nullptr;
        return false;
    }

    // Some plug-ins do not accept the component's serialized state in their
    // edit controller even though they can still create a usable editor. Treat
    // state synchronization as best-effort and attempt createView regardless.
    const auto sync_result = impl_->editor_controller->setComponentState(&component_state);
    const bool component_state_accepted = sync_result == Steinberg::kResultTrue;
    std::fprintf(stderr,
                 "[VST3 editor] setComponentState result=%d (%s).\n",
                 static_cast<int>(sync_result),
                 component_state_accepted ? "accepted" : "rejected");

    impl_->editor_view = Steinberg::owned(
        impl_->editor_controller->createView(Steinberg::Vst::ViewType::kEditor));
    if (!impl_->editor_view) {
        const auto parameter_count = impl_->editor_controller->getParameterCount();
        std::fprintf(stderr,
                     "[VST3 editor] createView(\"editor\") returned null; parameter count=%d. "
                     "Failure is before X11 embedding.\n",
                     parameter_count);
        error = "The VST3 edit controller returned no editor view from createView(\"editor\"). "
                "The controller reports " + std::to_string(parameter_count) +
                " parameters. Component state synchronization was " +
                (component_state_accepted ? std::string("accepted") : std::string("rejected")) +
                ". This is distinct from an unsupported X11 editor: the host has not "
                "received a view yet.";
        impl_->editor_controller->setComponentHandler(nullptr);
        impl_->editor_handler.reset();
        impl_->editor_controller = nullptr;
        return false;
    }

    std::fprintf(stderr, "[VST3 editor] createView succeeded; checking view size and X11 support.\n");
    Steinberg::ViewRect size{};
    const auto size_result = impl_->editor_view->getSize(&size);
    if (size_result != Steinberg::kResultTrue) {
        std::fprintf(stderr, "[VST3 editor] getSize failed (result=%d).\n",
                     static_cast<int>(size_result));
        error = "Could not determine the VST3 editor size.";
        close_editor();
        return false;
    }
    const auto platform_result = impl_->editor_view->isPlatformTypeSupported(
        Steinberg::kPlatformTypeX11EmbedWindowID);
    std::fprintf(stderr, "[VST3 editor] X11 embed support result=%d.\n",
                 static_cast<int>(platform_result));
    if (platform_result != Steinberg::kResultTrue) {
        error = "This plug-in editor does not support Linux X11 embedding. "
                "Wayland-native editor embedding is not implemented yet.";
        close_editor();
        return false;
    }

    impl_->editor_frame = std::make_unique<EditorFrame>();
    impl_->editor_view->setFrame(impl_->editor_frame.get());
    const auto attached_result = impl_->editor_view->attached(
        native_parent, Steinberg::kPlatformTypeX11EmbedWindowID);
    std::fprintf(stderr, "[VST3 editor] attached() result=%d.\n",
                 static_cast<int>(attached_result));
    if (attached_result != Steinberg::kResultTrue) {
        error = "The VST3 editor could not attach to the host window.";
        impl_->editor_view->setFrame(nullptr);
        close_editor();
        return false;
    }

    width = size.right - size.left;
    height = size.bottom - size.top;
    if (width <= 0 || height <= 0) {
        std::fprintf(stderr, "[VST3 editor] Invalid editor size %dx%d.\n", width, height);
        error = "The VST3 editor reported an invalid size.";
        close_editor();
        return false;
    }
    std::fprintf(stderr, "[VST3 editor] Host attachment succeeded; requested view size=%dx%d.\n",
                 width, height);
    return true;
#endif
}

void Vst3Processor::close_editor() noexcept {
#if defined(VST_HOST_ENABLE_VST3)
    if (impl_ && impl_->editor_view) {
        impl_->editor_view->setFrame(nullptr);
        impl_->editor_view->removed();
        impl_->editor_view = nullptr;
    }
    if (impl_ && impl_->editor_controller) {
        impl_->editor_controller->setComponentHandler(nullptr);
    }
    if (impl_) {
        impl_->editor_frame.reset();
        impl_->editor_handler.reset();
        impl_->editor_controller = nullptr;
    }
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
                         "VST3: getBusArrangement(input, %d) failed (result=%d).\n",
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
                         "VST3: getBusArrangement(output, %d) failed (result=%d).\n",
                         i, static_cast<int>(result));
            return false;
        }
    }

    // Prefer stereo, then fall back to mono main buses when the plug-in rejects stereo.
    constexpr Steinberg::int32 layouts[][2] = {{2, 2}, {1, 2}, {2, 1}, {1, 1}};
    bool arrangement_accepted = false;
    Steinberg::tresult arrangement_result = Steinberg::kResultFalse;
    for (const auto& layout : layouts) {
        input_arrangements[0] = layout[0] == 2
            ? Steinberg::Vst::SpeakerArr::kStereo : Steinberg::Vst::SpeakerArr::kMono;
        output_arrangements[0] = layout[1] == 2
            ? Steinberg::Vst::SpeakerArr::kStereo : Steinberg::Vst::SpeakerArr::kMono;
        arrangement_result = impl_->processor->setBusArrangements(
            input_arrangements.data(), input_count, output_arrangements.data(), output_count);
        if (arrangement_result == Steinberg::kResultOk) {
            impl_->input_channel_count = layout[0];
            impl_->output_channel_count = layout[1];
            arrangement_accepted = true;
            break;
        }
    }
    if (!arrangement_accepted) {
        std::fprintf(stderr,
                     "VST3: plugin rejected stereo and mono main bus arrangements "
                     "(inputs=%d, outputs=%d, result=%d).\n",
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
    impl_->input_bus.numChannels = impl_->input_channel_count;
    impl_->input_bus.channelBuffers32 = impl_->input_channels;
    impl_->output_bus = {};
    impl_->output_bus.numChannels = impl_->output_channel_count;
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
            const float left = samples[(offset + i) * 2];
            const float right = samples[(offset + i) * 2 + 1];
            impl_->input_left[i] = impl_->input_channel_count == 1
                ? 0.5F * (left + right) : left;
            impl_->input_right[i] = right;
            // If a plug-in reports a processing failure, preserve dry audio.
            impl_->output_left[i] = impl_->input_left[i];
            impl_->output_right[i] = impl_->output_channel_count == 1
                ? impl_->input_left[i] : impl_->input_right[i];
        }

        impl_->process_data.numSamples = static_cast<Steinberg::int32>(block_frames);
        impl_->process_context.projectTimeSamples = impl_->project_time_samples;
        const auto result = impl_->processor->process(impl_->process_data);
        if (result != Steinberg::kResultOk) {
            bool expected = false;
            if (impl_->process_failed.compare_exchange_strong(
                    expected, true, std::memory_order_relaxed)) {
                // Never write to stderr from the real-time callback.
                // The failure is latched atomically; keep audio passing through.
            }
            std::memcpy(impl_->output_left.data(), impl_->input_left.data(),
                        block_frames * sizeof(float));
            std::memcpy(impl_->output_right.data(), impl_->input_right.data(),
                        block_frames * sizeof(float));
        }

        for (std::size_t i = 0; i < block_frames; ++i) {
            samples[(offset + i) * 2] = impl_->output_left[i];
            samples[(offset + i) * 2 + 1] = impl_->output_channel_count == 1
                ? impl_->output_left[i] : impl_->output_right[i];
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
