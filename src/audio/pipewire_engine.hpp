#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace vst_host::audio {

class Vst3Processor;

struct PipeWireOptions {
    // Optional PipeWire node id/name for the microphone. If empty, the default
    // source is selected through PipeWire's autoconnect behavior.
    std::string input_target;
    std::string output_name{"vst-host-virtual-mic"};
    std::string output_description{"VST Host Virtual Microphone"};
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

private:
    struct Impl;
    Impl* impl_;
};

} // namespace vst_host::audio
