#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <string>

namespace vst_host::audio {

class Vst3Processor;

struct PipeWireOptions {
    // Optional PipeWire node id/name for the microphone. If empty, the default
    // source is selected through PipeWire's autoconnect behavior.
    std::string input_target;
    std::string output_name{"vst-host-virtual-mic"};
    std::string output_description{"VST Host Virtual Microphone"};
    // Ordered VST3 effects. Legacy single-plugin fields remain supported.
    std::vector<std::string> plugin_paths;
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
    [[nodiscard]] Vst3Processor* plugin_processor() const noexcept;
    [[nodiscard]] Vst3Processor* plugin_processor(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t plugin_count() const noexcept;
    [[nodiscard]] std::string plugin_path(std::size_t index) const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace vst_host::audio
