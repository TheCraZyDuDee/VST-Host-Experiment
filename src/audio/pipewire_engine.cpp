#include "pipewire_engine.hpp"

#include "stereo_ring_buffer.hpp"
#include "buffer_bounds.hpp"
#include "capture_target_policy.hpp"
#include "processor_chain.hpp"
#include "gain_processor.hpp"
#include "vst3_processor.hpp"
#include "vst2_processor.hpp"
#include "plugin_processor.hpp"
#include "plugin_discovery.hpp"
#include "windows_bridge_manager.hpp"
#include "windows_plugin_scope.hpp"

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

#include <algorithm>
#include <atomic>
#include <spa/param/audio/format-utils.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace vst_host::audio {
namespace {

constexpr std::uint32_t sample_rate = 48000;
constexpr std::uint32_t channel_count = 2;
constexpr std::size_t frame_bytes = sizeof(float) * channel_count;

struct EngineState {
    pw_main_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    pw_stream* capture_stream{};
    pw_stream* source_stream{};
    pw_registry* input_registry{};
    std::string selected_target_name;
    std::unordered_set<std::uint32_t> matching_target_nodes;
    std::atomic<bool> selected_target_present{false};
    std::atomic<bool> capture_streaming{false};
    StereoRingBuffer ring;
    StereoLevelMeter input_meter;
    StereoLevelMeter output_meter;
    GainProcessor processor;
    std::vector<std::shared_ptr<PluginProcessor>> plugin_chain;
    std::vector<std::string> plugin_paths;
    ProcessorChain processor_chain;
    std::size_t latency_frames{128};
    int setup_error{0};
    std::string setup_error_message;
};

void request_shutdown(void* userdata, int) {
    auto* state = static_cast<EngineState*>(userdata);
    if (state->loop != nullptr) {
        pw_main_loop_quit(state->loop);
    }
}

void on_capture_process(void* userdata) {
    auto* state = static_cast<EngineState*>(userdata);
    auto* buffer = pw_stream_dequeue_buffer(state->capture_stream);
    if (buffer == nullptr) {
        return;
    }

    auto* spa_buffer = buffer->buffer;
    if (spa_buffer != nullptr && spa_buffer->n_datas > 0 &&
        spa_buffer->datas[0].data != nullptr &&
        spa_buffer->datas[0].chunk != nullptr) {
        const auto& data = spa_buffer->datas[0];
        // Never interpret a malformed or truncated spa_chunk as sample data.
        // Skip invalid capture chunks; the output underrun path generates silence.
        if (valid_stereo_float_chunk(data.maxsize, data.chunk->offset,
                                     data.chunk->size)) {
            const auto frames = static_cast<std::size_t>(data.chunk->size) /
                                frame_bytes;
            const auto* samples = reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(data.data) + data.chunk->offset);
            // A pinned source must not silently process a different input.
            if (state->selected_target_name.empty() ||
                state->selected_target_present.load(std::memory_order_acquire)) {
                state->input_meter.observe(samples, frames);
                state->ring.push_interleaved(samples, frames);
            }
        }
    }

    pw_stream_queue_buffer(state->capture_stream, buffer);
}

void on_capture_state_changed(void* userdata, pw_stream_state, pw_stream_state state,
                              const char* error) {
    auto* data = static_cast<EngineState*>(userdata);
    data->capture_streaming.store(state == PW_STREAM_STATE_STREAMING,
                                  std::memory_order_release);
    if (state == PW_STREAM_STATE_ERROR) {
        data->setup_error_message = std::string("PipeWire capture stream error: ") +
            (error != nullptr ? error : "unknown error");
        std::fprintf(stderr, "%s\n", data->setup_error_message.c_str());
        data->setup_error = -EIO;
        if (data->loop != nullptr) {
            pw_main_loop_quit(data->loop);
        }
    }
}

void on_source_process(void* userdata) {
    auto* state = static_cast<EngineState*>(userdata);
    auto* buffer = pw_stream_dequeue_buffer(state->source_stream);
    if (buffer == nullptr) {
        return;
    }

    auto* spa_buffer = buffer->buffer;
    if (spa_buffer != nullptr && spa_buffer->n_datas > 0 &&
        spa_buffer->datas[0].data != nullptr &&
        spa_buffer->datas[0].chunk != nullptr) {
        auto& data = spa_buffer->datas[0];
        auto frames = static_cast<std::size_t>(data.maxsize) / frame_bytes;
        if (buffer->requested > 0) {
            frames = std::min(frames, static_cast<std::size_t>(buffer->requested));
        }
        auto* samples = reinterpret_cast<float*>(data.data);

        // Allow a small scheduling cushion: a tight two-period cap can discard
        // valid microphone samples on every callback with mismatched quanta.
        // Old audio is still trimmed if capture gets far ahead of playback.
        // Keep the virtual microphone available, but never relay another mic
        // or plugin-generated output while the explicitly pinned input is
        // absent. Don't replay buffered audio after it reconnects.
        const bool pinned_input_unavailable =
            !state->selected_target_name.empty() &&
            (!state->selected_target_present.load(std::memory_order_acquire) ||
             !state->capture_streaming.load(std::memory_order_acquire));
        if (pinned_input_unavailable) {
            mute_disconnected_capture(state->ring, samples, frames);
        } else {
            state->ring.pop_interleaved(samples, frames,
                std::max<std::size_t>(1024, state->latency_frames * 4));
            state->processor_chain.process(samples, frames);
            state->processor.process(samples, frames);
        }
        state->output_meter.observe(samples, frames);

        data.chunk->offset = 0;
        data.chunk->stride = static_cast<int32_t>(frame_bytes);
        data.chunk->size = static_cast<std::uint32_t>(frames * frame_bytes);
    }

    pw_stream_queue_buffer(state->source_stream, buffer);
}

void on_source_state_changed(void* userdata, pw_stream_state, pw_stream_state state,
                             const char* error) {
    auto* data = static_cast<EngineState*>(userdata);
    if (state == PW_STREAM_STATE_ERROR) {
        data->setup_error_message = std::string("PipeWire virtual microphone error: ") +
            (error != nullptr ? error : "unknown error");
        std::fprintf(stderr, "%s\n", data->setup_error_message.c_str());
        data->setup_error = -EIO;
        if (data->loop != nullptr) {
            pw_main_loop_quit(data->loop);
        }
    }
}

// Device node IDs can change after unplug/replug; node.name is the saved
// stable identity selected by the GUI. Registry notifications run on the
// PipeWire main-loop thread, not the real-time processing thread.
void on_input_registry_global(void* userdata, std::uint32_t id, std::uint32_t,
                              const char* type, std::uint32_t,
                              const spa_dict* properties) {
    auto* state = static_cast<EngineState*>(userdata);
    if (state->selected_target_name.empty() || !type || !properties ||
        std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

    const auto* node_name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
    const auto* media_class = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
    if (!node_name || !media_class ||
        state->selected_target_name != node_name ||
        (std::strcmp(media_class, "Audio/Source") != 0 &&
         std::strcmp(media_class, "Audio/Source/Virtual") != 0)) return;

    const bool was_available = !state->matching_target_nodes.empty();
    state->matching_target_nodes.insert(id);
    state->selected_target_present.store(true, std::memory_order_release);
    if (!was_available)
        std::fprintf(stderr, "Selected microphone connected: %s\n",
                     state->selected_target_name.c_str());
}

void on_input_registry_global_remove(void* userdata, std::uint32_t id) {
    auto* state = static_cast<EngineState*>(userdata);
    if (state->matching_target_nodes.erase(id) == 0 ||
        !state->matching_target_nodes.empty()) return;
    state->selected_target_present.store(false, std::memory_order_release);
    std::fprintf(stderr,
                 "Selected microphone unavailable; keeping virtual mic silent until %s returns.\n",
                 state->selected_target_name.c_str());
}

const pw_registry_events input_registry_events = [] {
    pw_registry_events events{};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = on_input_registry_global;
    events.global_remove = on_input_registry_global_remove;
    return events;
}();

const pw_stream_events capture_events = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = on_capture_state_changed;
    events.process = on_capture_process;
    return events;
}();

const pw_stream_events source_events = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = on_source_state_changed;
    events.process = on_source_process;
    return events;
}();

} // namespace

struct PipeWireEngine::Impl {
    explicit Impl(PipeWireOptions supplied_options)
        : options(std::move(supplied_options)) {
        state.processor.set_gain(options.gain);
        state.processor.set_bypassed(options.bypass);
        state.latency_frames = options.latency_frames;
        state.selected_target_name = options.input_target;
    }

    PipeWireOptions options;
    EngineState state;
    std::string last_error;
    std::vector<PluginState> plugin_snapshots;
    std::string state_warning;

    int fail(std::string message, int result) {
        last_error = std::move(message);
        std::fprintf(stderr, "%s\n", last_error.c_str());
        return result;
    }

    spa_hook capture_listener{};
    spa_hook source_listener{};
    spa_hook input_registry_listener{};
    bool initialized{false};
    std::atomic<pw_main_loop*> published_loop{nullptr};
    std::atomic<bool> published_running{false};
    std::atomic<PluginProcessor*> published_plugin{nullptr};

    int create_source_stream() {
        const auto latency = std::to_string(options.latency_frames) + "/48000";
        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Source",
            PW_KEY_MEDIA_ROLE, "Communication",
            // A pw_stream producing audio is the native PipeWire source API.
            // Unlike a pw_filter, it is exported as a capture source to clients.
            PW_KEY_MEDIA_CLASS, "Audio/Source",
            PW_KEY_NODE_NAME, options.output_name.c_str(),
            PW_KEY_NODE_DESCRIPTION, options.output_description.c_str(),
            PW_KEY_NODE_LATENCY, latency.c_str(),
            PW_KEY_AUDIO_CHANNELS, "2",
            PW_KEY_NODE_VIRTUAL, "true",
            nullptr);
        if (properties == nullptr) {
            return fail("Could not allocate PipeWire virtual microphone properties.", -ENOMEM);
        }

        state.source_stream = pw_stream_new(
            state.core, options.output_name.c_str(), properties);
        if (state.source_stream == nullptr)
            return fail("Could not create PipeWire virtual microphone stream.", -ENOMEM);

        pw_stream_add_listener(state.source_stream, &source_listener,
                               &source_events, &state);

        std::uint8_t pod_buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
        spa_audio_info_raw format{};
        format.format = SPA_AUDIO_FORMAT_F32;
        format.channels = channel_count;
        format.rate = sample_rate;
        format.position[0] = SPA_AUDIO_CHANNEL_FL;
        format.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[1];
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);

        const auto connect_result = pw_stream_connect(
            state.source_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS |
                                         PW_STREAM_FLAG_RT_PROCESS),
            params, 1);
        if (connect_result < 0)
            return fail(std::string("Could not publish virtual microphone: ") +
                        std::strerror(-connect_result), connect_result);
        return 0;
    }

    int create_capture_stream() {
        const auto latency = std::to_string(options.latency_frames) + "/48000";
        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Communication",
            PW_KEY_NODE_DESCRIPTION, "VST Host Microphone Capture",
            PW_KEY_NODE_LATENCY, latency.c_str(),
            nullptr);
        if (properties == nullptr)
            return fail("Could not allocate PipeWire capture stream properties.", -ENOMEM);
        const auto policy = capture_target_policy(options.input_target);
        if (policy.pinned) {
            pw_properties_set(properties, PW_KEY_TARGET_OBJECT,
                              options.input_target.c_str());
            // WirePlumber: do not link a missing target to another microphone.
            // Linger keeps our capture node alive until this SAME node.name
            // returns. Do not use node.dont-reconnect/PW_STREAM_FLAG_DONT_RECONNECT:
            // those destroy the stream instead of waiting for reconnection.
            pw_properties_set(properties, "node.dont-fallback", "true");
            pw_properties_set(properties, "node.linger", "true");
            pw_properties_set(properties, "node.dont-move", "true");
        }

        state.capture_stream = pw_stream_new(
            state.core, "vst-host-capture", properties);
        if (state.capture_stream == nullptr)
            return fail("Could not create PipeWire capture stream.", -ENOMEM);

        std::uint8_t pod_buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
        spa_audio_info_raw format{};
        format.format = SPA_AUDIO_FORMAT_F32;
        format.channels = channel_count;
        format.rate = sample_rate;
        format.position[0] = SPA_AUDIO_CHANNEL_FL;
        format.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[1];
        params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);

        pw_stream_add_listener(state.capture_stream, &capture_listener,
                               &capture_events, &state);

        const auto connect_result = pw_stream_connect(
            state.capture_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                         PW_STREAM_FLAG_MAP_BUFFERS |
                                         PW_STREAM_FLAG_RT_PROCESS),
            params, 1);
        if (connect_result < 0)
            return fail(std::string("Could not connect to microphone: ") +
                        std::strerror(-connect_result), connect_result);
        return 0;
    }
};

PipeWireEngine::PipeWireEngine(PipeWireOptions options)
    : impl_(new Impl(std::move(options))) {}

PipeWireEngine::~PipeWireEngine() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->published_plugin.store(nullptr, std::memory_order_release);
    impl_->published_loop.store(nullptr, std::memory_order_release);
    if (impl_->state.capture_stream != nullptr) {
        pw_stream_destroy(impl_->state.capture_stream);
    }
    if (impl_->state.input_registry != nullptr) {
        spa_hook_remove(&impl_->input_registry_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(impl_->state.input_registry));
    }
    if (impl_->state.source_stream != nullptr) {
        pw_stream_destroy(impl_->state.source_stream);
    }
    if (impl_->state.core != nullptr) {
        pw_core_disconnect(impl_->state.core);
    }
    if (impl_->state.context != nullptr) {
        pw_context_destroy(impl_->state.context);
    }
    if (impl_->state.loop != nullptr) {
        pw_main_loop_destroy(impl_->state.loop);
    }
    if (impl_->initialized) {
        pw_deinit();
    }
    delete impl_;
}

void PipeWireEngine::request_stop() noexcept {
    if (impl_ == nullptr) return;
    if (auto* loop = impl_->published_loop.load(std::memory_order_acquire)) {
        pw_main_loop_quit(loop);
    }
}

bool PipeWireEngine::is_running() const noexcept {
    return impl_ && impl_->published_running.load(std::memory_order_acquire);
}

bool PipeWireEngine::selected_input_available() const noexcept {
    if (!impl_) return false;
    if (impl_->state.selected_target_name.empty()) return true;
    return impl_->state.selected_target_present.load(std::memory_order_acquire) &&
           impl_->state.capture_streaming.load(std::memory_order_acquire);
}

PluginProcessor* PipeWireEngine::plugin_processor() const noexcept {
    return impl_ ? impl_->published_plugin.load(std::memory_order_acquire) : nullptr;
}

PluginProcessor* PipeWireEngine::plugin_processor(std::size_t index) const noexcept {
    // The engine builds these vectors on its worker thread. The first-plugin
    // pointer publishes their finalized contents with release semantics.
    if (!impl_ || !impl_->published_plugin.load(std::memory_order_acquire) ||
        index >= impl_->state.plugin_chain.size()) return nullptr;
    return impl_->state.plugin_chain[index].get();
}

bool PipeWireEngine::set_plugin_bypassed(std::size_t index, bool bypassed) noexcept {
    if (!impl_ || !impl_->published_plugin.load(std::memory_order_acquire))
        return false;
    return impl_->state.processor_chain.set_bypassed(index, bypassed);
}

std::size_t PipeWireEngine::plugin_count() const noexcept {
    return impl_ && impl_->published_plugin.load(std::memory_order_acquire)
        ? impl_->state.plugin_chain.size() : 0;
}

std::vector<PluginState> PipeWireEngine::saved_plugin_states() const {
    // The caller must first join the thread running PipeWireEngine::run().
    return impl_ ? impl_->plugin_snapshots : std::vector<PluginState>{};
}

std::string PipeWireEngine::plugin_state_warning() const {
    return impl_ ? impl_->state_warning : std::string{};
}

std::string PipeWireEngine::plugin_path(std::size_t index) const {
    if (!impl_ || !impl_->published_plugin.load(std::memory_order_acquire) ||
        index >= impl_->state.plugin_paths.size()) return {};
    return impl_->state.plugin_paths[index];
}

std::string PipeWireEngine::last_error() const {
    return impl_ ? impl_->last_error : std::string{};
}

AudioDiagnostics PipeWireEngine::diagnostics() const noexcept {
    if (!impl_) return {};
    return AudioDiagnostics{
        .dropped_frames = impl_->state.ring.dropped_frames(),
        .stale_frames = impl_->state.ring.stale_frames(),
        .underrun_frames = impl_->state.ring.underrun_frames(),
    };
}

AudioLevelReadings PipeWireEngine::level_readings() const noexcept {
    if (!impl_) return {};
    return AudioLevelReadings{
        .input = impl_->state.input_meter.reading(),
        .output = impl_->state.output_meter.reading(),
    };
}

int PipeWireEngine::run() {
    // A .vsthost file persists discovery roots so the GUI can repopulate its
    // library, but they are NOT additional processing-chain entries. Only
    // directory flags explicitly passed to the headless CLI populate these
    // non-persisted auto-load roots. Both frontends use the same DSP engine.
    const auto nativeAutoLoadRoots = impl_->options.auto_load_plugin_directories;
    const auto windowsAutoLoadRoots = impl_->options.auto_load_windows_plugin_directories;
    std::string bridge_error;
    std::string bridge_warning;
    WindowsBridgeSelection selected_bridge;
    if (!windowsAutoLoadRoots.empty()) {
        // GUI startup does not sync bridges: its selected chain references
        // already-prepared wrappers. Headless --config must behave the same.
        // Only an explicit --windows-plugin-dir enables global bridge sync.
        auto bridge_options = impl_->options;
        bridge_options.windows_plugin_directories = windowsAutoLoadRoots;
        if (!prepare_windows_vst3_plugins(
                bridge_options, bridge_error, &selected_bridge, &bridge_warning)) {
            impl_->last_error = bridge_error;
            std::fprintf(stderr, "Windows plugin setup failed: %s\n",
                         bridge_error.c_str());
            return -ENOENT;
        }
    }
    if (!bridge_warning.empty())
        std::fprintf(stderr, "%s", bridge_warning.c_str());

    int argc = 0;
    char* arguments[] = {nullptr};
    char** argv = arguments;
    pw_init(&argc, &argv);
    impl_->initialized = true;

    impl_->state.loop = pw_main_loop_new(nullptr);
    if (impl_->state.loop != nullptr) {
        impl_->published_loop.store(impl_->state.loop, std::memory_order_release);
    }
    if (impl_->state.loop == nullptr) {
        return impl_->fail("Could not create PipeWire main loop.", -ENOMEM);
    }

    impl_->state.context = pw_context_new(
        pw_main_loop_get_loop(impl_->state.loop), nullptr, 0);
    if (impl_->state.context == nullptr) {
        return impl_->fail("Could not create PipeWire context.", -ENOMEM);
    }
    impl_->state.core = pw_context_connect(impl_->state.context, nullptr, 0);
    if (impl_->state.core == nullptr) {
        return impl_->fail("Could not connect to PipeWire.", -EIO);
    }

    pw_loop_add_signal(pw_main_loop_get_loop(impl_->state.loop), SIGINT,
                       request_shutdown, &impl_->state);
    pw_loop_add_signal(pw_main_loop_get_loop(impl_->state.loop), SIGTERM,
                       request_shutdown, &impl_->state);

    if (!impl_->options.input_target.empty()) {
        impl_->state.input_registry = pw_core_get_registry(
            impl_->state.core, PW_VERSION_REGISTRY, 0);
        if (!impl_->state.input_registry)
            return impl_->fail(
                "Could not monitor the selected PipeWire microphone for reconnection.", -EIO);
        pw_registry_add_listener(impl_->state.input_registry,
                                 &impl_->input_registry_listener,
                                 &input_registry_events, &impl_->state);
    }

    auto plugin_paths = impl_->options.plugin_paths;
    // Portable .vsthost and CLI chains may still reference old yabridge
    // wrappers. After syncing a different backend, migrate only paths whose
    // equivalent new wrapper actually exists. Never alter unrelated plugins.
    if (!windowsAutoLoadRoots.empty()) {
        for (auto& path : plugin_paths)
            path = plugin_format(path) == PluginFormat::Vst2
                ? migrate_vst2_wrapper_path(path, selected_bridge)
                : detail::migrate_vst3_wrapper_path(path, selected_bridge);
    }
    auto preloaded_plugins = impl_->options.preloaded_plugins;
    if (plugin_paths.empty() && !impl_->options.plugin_path.empty())
        plugin_paths.push_back(
            windowsAutoLoadRoots.empty()
                ? impl_->options.plugin_path
                : (plugin_format(impl_->options.plugin_path) == PluginFormat::Vst2
                    ? migrate_vst2_wrapper_path(
                        impl_->options.plugin_path, selected_bridge)
                    : detail::migrate_vst3_wrapper_path(
                        impl_->options.plugin_path, selected_bridge)));
    if (preloaded_plugins.empty() && impl_->options.preloaded_plugin)
        preloaded_plugins.push_back(impl_->options.preloaded_plugin);
    std::string scan_warning;
    auto discovered_plugins = discover_vst3_plugins(nativeAutoLoadRoots, &scan_warning);
#if defined(VST_HOST_ENABLE_VST2)
    auto native_vst2 = discover_vst2_plugins(nativeAutoLoadRoots, &scan_warning);
    discovered_plugins.insert(discovered_plugins.end(),
                              native_vst2.begin(), native_vst2.end());
#endif
    if (!windowsAutoLoadRoots.empty()) {
        // The bridge's registry can include every Windows folder registered by
        // unrelated hosts. Search its generated wrappers, but select ONLY
        // originals inside the Windows roots explicitly passed on this CLI
        // invocation. Also scan those roots for inline-installed wrappers.
        std::vector<std::string> bridgeDirectories = windowsAutoLoadRoots;
        if (!selected_bridge.wrapper_directory.empty())
            bridgeDirectories.push_back(selected_bridge.wrapper_directory.string());
#if defined(VST_HOST_ENABLE_VST2)
        if (!selected_bridge.vst2_wrapper_directory.empty())
            bridgeDirectories.push_back(selected_bridge.vst2_wrapper_directory.string());
#endif
        auto bridgeVst3 = discover_vst3_plugins(bridgeDirectories, &scan_warning);
        std::size_t excluded = 0;
        bridgeVst3 = filter_bridge_plugins_to_selected_roots(
            bridgeVst3, windowsAutoLoadRoots, &excluded);
        discovered_plugins.insert(discovered_plugins.end(),
                                  bridgeVst3.begin(), bridgeVst3.end());
#if defined(VST_HOST_ENABLE_VST2)
        auto bridgeVst2 = discover_vst2_plugins(bridgeDirectories, &scan_warning);
        std::size_t excludedVst2 = 0;
        bridgeVst2 = filter_bridge_plugins_to_selected_roots(
            bridgeVst2, windowsAutoLoadRoots, &excludedVst2);
        excluded += excludedVst2;
        discovered_plugins.insert(discovered_plugins.end(),
                                  bridgeVst2.begin(), bridgeVst2.end());
#endif
        if (excluded > 0)
            std::fprintf(stderr, "Skipped %zu global Wine bridge wrapper(s) "
                         "outside explicitly selected Windows auto-load directories.\n",
                         excluded);
    }
    // Separate aliases are collapsed only for auto-discovery; explicitly
    // selected instances remain in the original processing order.
    auto discovered_entries = std::vector<std::pair<std::string, bool>>{};
    for (const auto& path : discovered_plugins)
        discovered_entries.emplace_back(path, false);
    discovered_entries = deduplicate_vst3_browser_entries(discovered_entries);
    if (!scan_warning.empty()) std::fprintf(stderr, "%s", scan_warning.c_str());
    // Keep intentional repeated explicit plugin instances, but never append
    // discovered bridge aliases whose original Windows module is already
    // represented in the CLI chain. This also covers overlapping scan roots.
    std::unordered_set<std::string> known_modules;
    for (const auto& path : plugin_paths)
        known_modules.insert(plugin_source_identity(path));
    std::size_t added_by_directory_discovery = 0;
    for (const auto& entry : discovered_entries) {
        if (known_modules.insert(plugin_source_identity(entry.first)).second) {
            plugin_paths.push_back(entry.first);
            ++added_by_directory_discovery;
        }
    }
    if (!nativeAutoLoadRoots.empty() || !windowsAutoLoadRoots.empty())
        std::printf("CLI directory auto-load: %zu additional plugin(s)\n",
                    added_by_directory_discovery);
    if (preloaded_plugins.size() > plugin_paths.size()) {
        return impl_->fail(
            "More preloaded VST3 instances than plugin paths were supplied.", -EINVAL);
    }
    impl_->state.plugin_chain.reserve(plugin_paths.size());
    for (std::size_t index = 0; index < plugin_paths.size(); ++index) {
        std::shared_ptr<PluginProcessor> plugin;
        if (index < preloaded_plugins.size() && preloaded_plugins[index])
            plugin = preloaded_plugins[index];
        if (!plugin) {
            if (plugin_format(plugin_paths[index]) == PluginFormat::Vst2)
                plugin = std::make_shared<Vst2Processor>(plugin_paths[index]);
            else if (plugin_format(plugin_paths[index]) == PluginFormat::Vst3)
                plugin = std::make_shared<Vst3Processor>(plugin_paths[index]);
            else
                return impl_->fail("Unsupported plugin format: " + plugin_paths[index],
                                   -EINVAL);
        }
        std::string plugin_error;
        if (!plugin->load(plugin_error)) {
            return impl_->fail("Could not load plugin " +
                std::to_string(index + 1) + " ('" + plugin_paths[index] +
                "'): " + plugin_error, -EINVAL);
        }
        if (index < impl_->options.plugin_states.size() &&
            !impl_->options.plugin_states[index].empty() &&
            !plugin->restore_plugin_state(impl_->options.plugin_states[index], plugin_error)) {
            return impl_->fail("Could not restore saved state of plugin " +
                std::to_string(index + 1) + " ('" + plugin_paths[index] +
                "'): " + plugin_error +
                ". Clear the saved state for this effect if its version has changed.",
                -EINVAL);
        }
        impl_->state.processor_chain.add(*plugin);
        if (index < impl_->options.plugin_bypassed.size())
            (void)impl_->state.processor_chain.set_bypassed(
                index, impl_->options.plugin_bypassed[index]);
        impl_->state.plugin_paths.push_back(plugin_paths[index]);
        impl_->state.plugin_chain.push_back(std::move(plugin));
    }

    const AudioProcessSpec process_spec{sample_rate, channel_count, 1024};
    if (!impl_->state.processor.prepare(process_spec)) {
        return impl_->fail("Could not prepare the output gain processor.", -EINVAL);
    }
    if (!impl_->state.processor_chain.prepare(process_spec)) {
        return impl_->fail(
            "Could not prepare plugin chain. Check plugin format and audio I/O support.",
            -EINVAL);
    }

    auto result = impl_->create_source_stream();
    if (result == 0) {
        result = impl_->create_capture_stream();
    }
    if (result == 0 && !impl_->state.plugin_chain.empty()) {
        impl_->published_plugin.store(impl_->state.plugin_chain.front().get(), std::memory_order_release);
    }
    if (result == 0) {
        std::printf("Virtual microphone: %s\n", impl_->options.output_name.c_str());
        std::printf("Input: %s\n",
                    impl_->options.input_target.empty() ? "default microphone"
                                                        : impl_->options.input_target.c_str());
        std::printf("Gain: %.3f%s\n", impl_->options.gain,
                    impl_->options.bypass ? " (bypassed)" : "");
        std::printf("Requested latency: %u frames (%.2f ms at 48 kHz)\n",
                    impl_->options.latency_frames,
                    1000.0 * static_cast<double>(impl_->options.latency_frames) / sample_rate);
        for (std::size_t index = 0; index < impl_->state.plugin_chain.size(); ++index) {
            const auto& plugin = impl_->state.plugin_chain[index];
            std::printf("Plugin chain %zu: %s%s (latency %u frames)\n",
                        index + 1, plugin->plugin_path().c_str(),
                        impl_->state.processor_chain.is_bypassed(index) ? " [BYPASSED]" : "",
                        plugin->latency_frames());
        }
        std::printf("Press Ctrl+C to stop.\n");
        impl_->published_running.store(true, std::memory_order_release);
        pw_main_loop_run(impl_->state.loop);
        impl_->published_running.store(false, std::memory_order_release);

        // Stop realtime callbacks before calling VST3 getState(), which may
        // allocate memory, access files, and query Wine bridge processes.
        // The GUI closes the editor before requesting shutdown.
        if (impl_->state.capture_stream)
            (void)pw_stream_disconnect(impl_->state.capture_stream);
        if (impl_->state.source_stream)
            (void)pw_stream_disconnect(impl_->state.source_stream);

        impl_->plugin_snapshots = impl_->options.plugin_states;
        impl_->plugin_snapshots.resize(impl_->state.plugin_chain.size());
        for (std::size_t index = 0; index < impl_->state.plugin_chain.size(); ++index) {
            PluginState snapshot;
            std::string state_error;
            if (impl_->state.plugin_chain[index]->snapshot_plugin_state(snapshot, state_error)) {
                impl_->plugin_snapshots[index] = std::move(snapshot);
            } else {
                impl_->state_warning += "Could not save state for " +
                    impl_->state.plugin_paths[index] + ": " + state_error + "\n";
            }
        }
        if (!impl_->state_warning.empty())
            std::fprintf(stderr, "%s", impl_->state_warning.c_str());

        result = impl_->state.setup_error;
        if (result != 0) {
            impl_->last_error = impl_->state.setup_error_message.empty()
                ? "PipeWire stream stopped with an error. See the terminal for details."
                : impl_->state.setup_error_message;
        }
        const auto dropped = impl_->state.ring.dropped_frames();
        const auto underruns = impl_->state.ring.underrun_frames();
        const auto stale = impl_->state.ring.stale_frames();
        std::printf("Audio diagnostics: overflow=%llu frames (%.2f ms), underrun=%llu frames (%.2f ms), stale-trimmed=%llu frames (%.2f ms)\n",
                    static_cast<unsigned long long>(dropped),
                    1000.0 * static_cast<double>(dropped) / sample_rate,
                    static_cast<unsigned long long>(underruns),
                    1000.0 * static_cast<double>(underruns) / sample_rate,
                    static_cast<unsigned long long>(stale),
                    1000.0 * static_cast<double>(stale) / sample_rate);
    }

    return result;
}

} // namespace vst_host::audio
