#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <string>

namespace vst_host::audio {

class Vst3Processor;

struct AudioDiagnostics {
    std::uint64_t dropped_frames{};
    std::uint64_t stale_frames{};
    std::uint64_t underrun_frames{};
};

struct PipeWireOptions {
    // Stable PipeWire node.name for the selected microphone. A nonempty target
    // is strictly pinned: no fallback to a different mic on unplug, and the
    // virtual mic remains silent until this same node.name returns. Empty
    // means explicitly follow PipeWire's default capture source.
    std::string input_target;
    std::string output_name{"vst-host-virtual-mic"};
    std::string output_description{"VST Host Virtual Microphone"};
    // Ordered VST3 effects. Legacy single-plugin fields remain supported.
    std::vector<std::string> plugin_paths;
    // Per-instance bypass (false by default); aligned with plugin_paths.
    // Once loaded, bypass may be changed atomically without restarting audio.
    std::vector<bool> plugin_bypassed;
    // Search roots for native Linux VST3 bundle directories.
    std::vector<std::string> plugin_directories;
    // Windows VST3 directories managed by optional Wine/yabridge integration.
    std::vector<std::string> windows_plugin_directories;
    std::vector<std::shared_ptr<Vst3Processor>> preloaded_plugins;
    std::string plugin_path;
    std::shared_ptr<Vst3Processor> preloaded_plugin;
    float gain{1.0F};
    std::uint32_t latency_frames{128};
    bool bypass{false};
};

class PipeWireEngine {
public:
    explicit PipeWireEngine(PipeWireOptions options);
    PipeWireEngine(const PipeWireEngine&) = delete;
    PipeWireEngine& operator=(const PipeWireEngine&) = delete;
    ~PipeWireEngine();

    // Blocks until SIGINT/SIGTERM or an internal setup failure.
    int run();
    void request_stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept;
    // When a named source is selected: false until its exact node.name is
    // present and the capture stream is active. An empty/default input is
    // intentionally not pinned and always reports true.
    [[nodiscard]] bool selected_input_available() const noexcept;
    [[nodiscard]] Vst3Processor* plugin_processor() const noexcept;
    [[nodiscard]] Vst3Processor* plugin_processor(std::size_t index) const noexcept;
    // Safe to call from the UI while PipeWire audio processing is active.
    bool set_plugin_bypassed(std::size_t index, bool bypassed) noexcept;
    [[nodiscard]] std::size_t plugin_count() const noexcept;
    [[nodiscard]] std::string plugin_path(std::size_t index) const;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] AudioDiagnostics diagnostics() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace vst_host::audio
