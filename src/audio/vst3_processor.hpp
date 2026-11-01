#pragma once

#include "plugin_processor.hpp"

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace vst_host::audio {

// Hosts one native Linux VST3 audio effect. The SDK is optional at build time;
// load() reports a clear error when the host was built without VST3 support.
class Vst3Processor final : public PluginProcessor {
public:
    explicit Vst3Processor(std::string plugin_path);
    ~Vst3Processor() override;

    Vst3Processor(const Vst3Processor&) = delete;
    Vst3Processor& operator=(const Vst3Processor&) = delete;

    bool load(std::string& error) override;
    // Restore after load(), before prepare(). Snapshot only AFTER PipeWire
    // callbacks have stopped and the native editor has been detached.
    bool restore_plugin_state(const PluginState& state, std::string& error) override;
    bool snapshot_plugin_state(PluginState& state, std::string& error) override;
    bool open_editor(void* native_parent, int& width, int& height, std::string& error) override;
    void close_editor() noexcept override;

    // Called from the GUI thread while the editor is open. Parameter edits
    // are transferred to the real-time thread using a bounded SPSC queue.
    using ParameterInfo = PluginProcessor::ParameterInfo;
    [[nodiscard]] std::vector<ParameterInfo> editor_parameters() const override;
    bool set_editor_parameter(std::uint32_t id, double normalized_value) override;
    bool prepare(const AudioProcessSpec& spec) override;
    void process(float* interleaved_samples, std::size_t frames) noexcept override;
    void reset() noexcept override;
    [[nodiscard]] std::uint32_t latency_frames() const noexcept override;

    [[nodiscard]] const std::string& plugin_path() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vst_host::audio
