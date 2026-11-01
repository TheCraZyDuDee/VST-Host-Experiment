#pragma once

#include "plugin_processor.hpp"

#include <memory>
#include <string>

namespace vst_host::audio {

// Optional legacy VST2 host. A user-supplied, appropriately licensed VST2.4
// SDK is required at compile time. No Steinberg VST2 SDK files are included
// or automatically fetched by this repository.
class Vst2Processor final : public PluginProcessor {
public:
    explicit Vst2Processor(std::string path);
    ~Vst2Processor() override;
    bool load(std::string& error) override;
    bool restore_plugin_state(const PluginState&, std::string& error) override;
    bool snapshot_plugin_state(PluginState&, std::string& error) override;
    bool open_editor(void* native_parent, int& width, int& height,
                     std::string& error) override;
    void close_editor() noexcept override;
    void idle_editor() noexcept override;
    bool has_native_editor() const noexcept override;
    std::vector<ParameterInfo> editor_parameters() const override;
    bool set_editor_parameter(std::uint32_t id, double value) override;
    bool prepare(const AudioProcessSpec& spec) override;
    void process(float* samples, std::size_t frames) noexcept override;
    void reset() noexcept override;
    std::uint32_t latency_frames() const noexcept override;
    const std::string& plugin_path() const noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vst_host::audio
