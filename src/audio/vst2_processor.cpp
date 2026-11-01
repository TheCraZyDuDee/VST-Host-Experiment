#include "vst2_processor.hpp"
#include "parameter_event_queue.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#if defined(VST_HOST_ENABLE_VST2)
// Some original VST2.4 aeffect.h distributions expand VSTCALLBACK to
// __cdecl unconditionally, including on Linux. GCC/Clang on Linux do not
// define that MSVC spelling; the platform's default C calling convention is
// already correct for the VST2 ABI. Keep this compatibility shim local to
// the legacy SDK translation unit instead of modifying the user's SDK.
#if defined(__linux__) && (defined(__GNUC__) || defined(__clang__)) && !defined(__cdecl)
#define __cdecl
#endif
#include <aeffectx.h> // User-supplied legacy SDK, never redistributed.
#include <dlfcn.h>
#endif

namespace vst_host::audio {

struct Vst2Processor::Impl {
    explicit Impl(std::string plugin) : path(std::move(plugin)) {}
    std::string path;
    std::uint32_t latency{};
#if defined(VST_HOST_ENABLE_VST2)
    void* library{};
    AEffect* effect{};
    bool prepared{};
    bool editor_open{};
    bool active{};
    std::vector<float> input_left, input_right, output_left, output_right;
    float* input_channels[2]{};
    float* output_channels[2]{};
    ParameterEventQueue pending;
    std::uint32_t max_block{1024};

    static VstIntPtr VSTCALLBACK host(AEffect*, VstInt32 opcode, VstInt32,
                                     VstIntPtr, void* ptr, float) {
        switch (opcode) {
            case audioMasterVersion: return 2400;
            case audioMasterGetSampleRate: return 48000;
            case audioMasterGetBlockSize: return 1024;
            case audioMasterGetVendorString:
                if (ptr) { std::strcpy(static_cast<char*>(ptr), "VST Host Experiment"); return 1; }
                return 0;
            case audioMasterGetProductString:
                if (ptr) { std::strcpy(static_cast<char*>(ptr), "VST Host Experiment"); return 1; }
                return 0;
            case audioMasterCanDo: return 0;
            default: return 0;
        }
    }

    VstIntPtr dispatch(VstInt32 opcode, VstInt32 index = 0, VstIntPtr value = 0,
                       void* ptr = nullptr, float opt = 0.0F) const noexcept {
        return effect && effect->dispatcher
            ? effect->dispatcher(effect, opcode, index, value, ptr, opt) : 0;
    }
#endif
};

Vst2Processor::Vst2Processor(std::string path)
    : impl_(std::make_unique<Impl>(std::move(path))) {}

Vst2Processor::~Vst2Processor() {
    close_editor();
#if defined(VST_HOST_ENABLE_VST2)
    if (impl_->effect) {
        if (impl_->active) impl_->dispatch(effMainsChanged, 0, 0);
        impl_->dispatch(effClose);
        impl_->effect = nullptr;
    }
    if (impl_->library) dlclose(impl_->library);
#endif
}

bool Vst2Processor::load(std::string& error) {
#if !defined(VST_HOST_ENABLE_VST2)
    error = "VST2 support was not built. Configure with -DVST_HOST_ENABLE_VST2=ON "
            "and a legally licensed VST2_SDK_ROOT.";
    return false;
#else
    if (impl_->effect) return true;
    impl_->library = dlopen(impl_->path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!impl_->library) {
        const char* message = dlerror();
        error = std::string("Could not load VST2 library: ") +
                (message ? message : impl_->path);
        return false;
    }
    using Entry = AEffect* (VSTCALLBACK*)(audioMasterCallback);
    Entry entry{};
    for (const char* name : {"VSTPluginMain", "main", "MAIN", "PluginMain"}) {
        void* symbol = dlsym(impl_->library, name);
        if (symbol) { entry = reinterpret_cast<Entry>(symbol); break; }
    }
    if (!entry) {
        error = "No VSTPluginMain entry point found in " + impl_->path;
        return false;
    }
    impl_->effect = entry(&Impl::host);
    if (!impl_->effect || impl_->effect->magic != kEffectMagic ||
        !impl_->effect->dispatcher || !impl_->effect->processReplacing ||
        !(impl_->effect->flags & effFlagsCanReplacing)) {
        error = "Invalid or unsupported VST2.4 effect (32-bit float replacing required): " +
                impl_->path;
        impl_->effect = nullptr;
        return false;
    }
    impl_->dispatch(effOpen);
    if (impl_->effect->numInputs < 1 || impl_->effect->numInputs > 2 ||
        impl_->effect->numOutputs < 1 || impl_->effect->numOutputs > 2) {
        error = "VST2 effect must have mono/stereo audio inputs and outputs: " + impl_->path;
        return false;
    }
    return true;
#endif
}

bool Vst2Processor::prepare(const AudioProcessSpec& spec) {
#if !defined(VST_HOST_ENABLE_VST2)
    (void)spec;
    return false;
#else
    if (!impl_->effect || spec.sample_rate == 0 || spec.channels != 2 ||
        spec.max_block_size == 0 || impl_->active) return false;
    const auto frames = static_cast<std::size_t>(spec.max_block_size);
    impl_->max_block = spec.max_block_size;
    impl_->input_left.assign(frames, 0.0F);
    impl_->input_right.assign(frames, 0.0F);
    impl_->output_left.assign(frames, 0.0F);
    impl_->output_right.assign(frames, 0.0F);
    impl_->input_channels[0] = impl_->input_left.data();
    impl_->input_channels[1] = impl_->input_right.data();
    impl_->output_channels[0] = impl_->output_left.data();
    impl_->output_channels[1] = impl_->output_right.data();
    impl_->dispatch(effSetSampleRate, 0, 0, nullptr, static_cast<float>(spec.sample_rate));
    impl_->dispatch(effSetBlockSize, 0, static_cast<VstIntPtr>(spec.max_block_size));
    impl_->dispatch(effMainsChanged, 0, 1);
    impl_->active = true;
    impl_->prepared = true;
    impl_->latency = static_cast<std::uint32_t>(
        std::max<VstInt32>(0, impl_->effect->initialDelay));
    return true;
#endif
}

void Vst2Processor::process(float* samples, std::size_t frames) noexcept {
#if defined(VST_HOST_ENABLE_VST2)
    if (!impl_->prepared || !impl_->effect || !samples) return;
    std::size_t offset{};
    while (offset < frames) {
        const auto count = std::min(frames - offset,
                                     static_cast<std::size_t>(impl_->max_block));
        for (std::size_t i = 0; i < count; ++i) {
            const float left = samples[2 * (offset + i)];
            const float right = samples[2 * (offset + i) + 1];
            impl_->input_left[i] = impl_->effect->numInputs == 1
                ? 0.5F * (left + right) : left;
            impl_->input_right[i] = right;
            impl_->output_left[i] = impl_->input_left[i];
            impl_->output_right[i] = impl_->input_right[i];
        }
        ParameterEventQueue::Event change{};
        for (int applied = 0; applied < 64 && impl_->pending.pop(change); ++applied) {
            if (change.id < static_cast<std::uint32_t>(impl_->effect->numParams))
                impl_->effect->setParameter(impl_->effect,
                                            static_cast<VstInt32>(change.id),
                                            static_cast<float>(change.normalized_value));
        }
        impl_->effect->processReplacing(impl_->effect,
                                       impl_->input_channels, impl_->output_channels,
                                       static_cast<VstInt32>(count));
        for (std::size_t i = 0; i < count; ++i) {
            samples[2 * (offset + i)] = impl_->output_left[i];
            samples[2 * (offset + i) + 1] = impl_->effect->numOutputs == 1
                ? impl_->output_left[i] : impl_->output_right[i];
        }
        offset += count;
    }
#else
    (void)samples; (void)frames;
#endif
}

void Vst2Processor::reset() noexcept {
    // The VST2 2.4 API has no universally supported RT-safe reset request.
}
std::uint32_t Vst2Processor::latency_frames() const noexcept { return impl_->latency; }
const std::string& Vst2Processor::plugin_path() const noexcept { return impl_->path; }

bool Vst2Processor::open_editor(void* parent, int& width, int& height, std::string& error) {
#if !defined(VST_HOST_ENABLE_VST2)
    (void)parent; (void)width; (void)height;
    error = "VST2 support is disabled.";
    return false;
#else
    if (!impl_->effect || !parent) {
        error = "VST2 editor requires a loaded effect and an X11 parent.";
        return false;
    }
    if (impl_->editor_open) return true;
    width = 560;
    height = 320;
    if (impl_->effect->flags & effFlagsHasEditor) {
        ERect* rectangle = nullptr;
        impl_->dispatch(effEditGetRect, 0, 0, &rectangle);
        if (rectangle && rectangle->right > rectangle->left &&
            rectangle->bottom > rectangle->top) {
            width = std::clamp<int>(rectangle->right - rectangle->left, 200, 3000);
            height = std::clamp<int>(rectangle->bottom - rectangle->top, 150, 2000);
        }
        impl_->dispatch(effEditOpen, 0, 0, parent);
    }
    // Plugins without an editor can still be controlled using Host Controls.
    impl_->editor_open = true;
    return true;
#endif
}

void Vst2Processor::close_editor() noexcept {
#if defined(VST_HOST_ENABLE_VST2)
    if (impl_->effect && impl_->editor_open && (impl_->effect->flags & effFlagsHasEditor))
        impl_->dispatch(effEditClose);
    impl_->editor_open = false;
#endif
}

void Vst2Processor::idle_editor() noexcept {
#if defined(VST_HOST_ENABLE_VST2)
    if (impl_->effect && impl_->editor_open && (impl_->effect->flags & effFlagsHasEditor))
        impl_->dispatch(effEditIdle);
#endif
}

bool Vst2Processor::has_native_editor() const noexcept {
#if defined(VST_HOST_ENABLE_VST2)
    return impl_->effect && (impl_->effect->flags & effFlagsHasEditor) != 0;
#else
    return false;
#endif
}

std::vector<PluginProcessor::ParameterInfo> Vst2Processor::editor_parameters() const {
    std::vector<ParameterInfo> result;
#if defined(VST_HOST_ENABLE_VST2)
    if (!impl_->effect) return result;
    const int count = std::clamp<int>(impl_->effect->numParams, 0, 512);
    result.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        char name[256]{};
        impl_->dispatch(effGetParamName, index, 0, name);
        ParameterInfo info;
        info.id = static_cast<std::uint32_t>(index);
        for (int i = 0; name[i] && i < 255; ++i)
            info.name.push_back(static_cast<char16_t>(
                static_cast<unsigned char>(name[i])));
        info.normalized_value = std::clamp<double>(
            impl_->effect->getParameter(impl_->effect, index), 0.0, 1.0);
        result.push_back(std::move(info));
    }
#endif
    return result;
}

bool Vst2Processor::set_editor_parameter(std::uint32_t id, double value) {
#if !defined(VST_HOST_ENABLE_VST2)
    (void)id; (void)value;
    return false;
#else
    if (!impl_->effect || id >= static_cast<std::uint32_t>(impl_->effect->numParams) ||
        !std::isfinite(value) || value < 0.0 || value > 1.0) return false;
    // The UI is a single producer; effect->setParameter() runs on the audio
    // callback to avoid concurrent mutation of the VST2 DSP.
    return impl_->pending.push(id, value);
#endif
}

// Store either a bank chunk or a bounded list of parameter values, prefixed
// with a small format marker and the plugin's unique ID. No Steinberg SDK
// structs are serialized. All snapshot calls are outside real-time processing.
bool Vst2Processor::snapshot_plugin_state(PluginState& state, std::string& error) {
#if !defined(VST_HOST_ENABLE_VST2)
    (void)state;
    error = "VST2 support is disabled.";
    return false;
#else
    if (!impl_->effect) { error = "VST2 effect is not loaded."; return false; }
    PluginState snapshot;
    auto& data = snapshot.component;
    auto append_u32 = [&](std::uint32_t x) {
        for (int i = 0; i < 4; ++i) data.push_back(static_cast<std::uint8_t>(x >> (i * 8)));
    };
    if (impl_->effect->flags & effFlagsProgramChunks) {
        void* chunk = nullptr;
        const auto count = impl_->dispatch(effGetChunk, 0, 0, &chunk);
        if (count <= 0 || !chunk ||
            static_cast<std::size_t>(count) > max_plugin_state_bytes - 8) {
            error = "VST2 plugin could not provide a valid state chunk.";
            return false;
        }
        data = {'V', '2', 'C', '1'};
        append_u32(static_cast<std::uint32_t>(impl_->effect->uniqueID));
        const auto* bytes = static_cast<const std::uint8_t*>(chunk);
        data.insert(data.end(), bytes, bytes + count);
    } else {
        const int count = impl_->effect->numParams;
        if (count < 0 || count > 65536) {
            error = "Invalid VST2 parameter count.";
            return false;
        }
        data = {'V', '2', 'P', '1'};
        append_u32(static_cast<std::uint32_t>(impl_->effect->uniqueID));
        append_u32(static_cast<std::uint32_t>(count));
        for (int i = 0; i < count; ++i) {
            const float value = impl_->effect->getParameter(impl_->effect, i);
            append_u32(std::bit_cast<std::uint32_t>(
                std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F));
        }
    }
    state = std::move(snapshot);
    return true;
#endif
}

bool Vst2Processor::restore_plugin_state(const PluginState& state, std::string& error) {
#if !defined(VST_HOST_ENABLE_VST2)
    (void)state;
    error = "VST2 support is disabled.";
    return false;
#else
    if (state.empty()) return true;
    if (!impl_->effect || state.component.size() < 8) {
        error = "Incomplete VST2 state or plugin not loaded.";
        return false;
    }
    const auto& d = state.component;
    const auto read_u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(d[at]) |
               (static_cast<std::uint32_t>(d[at + 1]) << 8) |
               (static_cast<std::uint32_t>(d[at + 2]) << 16) |
               (static_cast<std::uint32_t>(d[at + 3]) << 24);
    };
    if (read_u32(4) != static_cast<std::uint32_t>(impl_->effect->uniqueID)) {
        error = "VST2 state was saved for a different effect ID.";
        return false;
    }
    const bool chunk = d[0] == 'V' && d[1] == '2' && d[2] == 'C' && d[3] == '1';
    const bool params = d[0] == 'V' && d[1] == '2' && d[2] == 'P' && d[3] == '1';
    if (chunk && d.size() > 8 && d.size() <= max_plugin_state_bytes) {
        impl_->dispatch(effSetChunk, 0, static_cast<VstIntPtr>(d.size() - 8),
                        const_cast<std::uint8_t*>(d.data() + 8));
        return true;
    }
    if (params && d.size() >= 12 &&
        read_u32(8) == static_cast<std::uint32_t>(impl_->effect->numParams) &&
        d.size() == 12 + static_cast<std::size_t>(impl_->effect->numParams) * 4) {
        for (int i = 0; i < impl_->effect->numParams; ++i) {
            const float value = std::bit_cast<float>(read_u32(12 + 4 * static_cast<std::size_t>(i)));
            if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
                error = "VST2 saved parameter value is invalid.";
                return false;
            }
        }
        for (int i = 0; i < impl_->effect->numParams; ++i)
            impl_->effect->setParameter(impl_->effect, i,
                std::bit_cast<float>(read_u32(12 + 4 * static_cast<std::size_t>(i))));
        return true;
    }
    error = "Unsupported or malformed VST2 state payload.";
    return false;
#endif
}

} // namespace vst_host::audio
