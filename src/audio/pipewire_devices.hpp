#pragma once

#include <pipewire/pipewire.h>

#include <algorithm>
#include <string>
#include <vector>

namespace vst_host::audio {

struct PipeWireInputDevice {
    std::uint32_t id{};
    std::string name;
    std::string description;
};

namespace detail {

struct DeviceEnumerationState {
    pw_main_loop* loop{};
    int pending{-1};
    bool done{false};
    std::string error;
    std::vector<PipeWireInputDevice> devices;
};

inline void on_registry_global(void* userdata, std::uint32_t id, std::uint32_t,
                               const char* type, std::uint32_t,
                               const spa_dict* properties) {
    auto* state = static_cast<DeviceEnumerationState*>(userdata);
    if (!type || std::string(type) != PW_TYPE_INTERFACE_Node || !properties) return;
    const char* media_class = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
    if (!media_class ||
        (std::string(media_class) != "Audio/Source" &&
         std::string(media_class) != "Audio/Source/Virtual")) return;

    const char* name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
    if (!name || !*name) return;
    const char* description = spa_dict_lookup(properties, PW_KEY_NODE_DESCRIPTION);
    state->devices.push_back({id, name, description && *description ? description : name});
}

inline void on_core_done(void* userdata, std::uint32_t id, int seq) {
    auto* state = static_cast<DeviceEnumerationState*>(userdata);
    if (id == PW_ID_CORE && seq == state->pending) {
        state->done = true;
        pw_main_loop_quit(state->loop);
    }
}

inline void on_core_error(void* userdata, std::uint32_t id, int, int result,
                          const char* message) {
    auto* state = static_cast<DeviceEnumerationState*>(userdata);
    if (id == PW_ID_CORE) {
        state->error = message ? message : "PipeWire registry request failed";
        if (result < 0) state->error += " (" + std::to_string(result) + ")";
        state->done = true;
        pw_main_loop_quit(state->loop);
    }
}

inline const pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
};

inline const pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

} // namespace detail

// Enumerate current capture sources synchronously. This is intended for an
// explicit GUI refresh action, never for the audio processing thread.
inline std::vector<PipeWireInputDevice> list_pipewire_input_devices(std::string& error) {
    int argc = 0;
    char* arguments[] = {nullptr};
    char** argv = arguments;
    pw_init(&argc, &argv);

    auto* loop = pw_main_loop_new(nullptr);
    auto* context = loop ? pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0) : nullptr;
    auto* core = context ? pw_context_connect(context, nullptr, 0) : nullptr;
    auto* registry = core ? pw_core_get_registry(core, PW_VERSION_REGISTRY, 0) : nullptr;
    if (!loop || !context || !core || !registry) {
        error = "Could not connect to the PipeWire registry.";
        if (registry) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
        if (core) pw_core_disconnect(core);
        if (context) pw_context_destroy(context);
        if (loop) pw_main_loop_destroy(loop);
        pw_deinit();
        return {};
    }

    detail::DeviceEnumerationState state;
    state.loop = loop;
    spa_hook registry_listener{};
    spa_hook core_listener{};
    pw_registry_add_listener(registry, &registry_listener, &detail::registry_events, &state);
    pw_core_add_listener(core, &core_listener, &detail::core_events, &state);
    state.pending = pw_core_sync(core, PW_ID_CORE, 0);
    if (state.pending < 0) {
        error = "Could not request PipeWire device enumeration.";
    } else {
        while (!state.done) pw_main_loop_run(loop);
        error = state.error;
    }

    spa_hook_remove(&core_listener);
    spa_hook_remove(&registry_listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    pw_deinit();

    std::sort(state.devices.begin(), state.devices.end(),
              [](const auto& left, const auto& right) {
                  if (left.description != right.description)
                      return left.description < right.description;
                  return left.name < right.name;
              });
    return state.devices;
}

} // namespace vst_host::audio
