#pragma once

#include "audio_processor.hpp"

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace vst_host::audio {

// Hosts one native Linux VST3 audio effect. The SDK is optional at build time;
// load() reports a clear error when the host was built without VST3 support.
class Vst3Processor final : public AudioProcessor {
public:
    explicit Vst3Processor(std::string plugin_path);
    ~Vst3Processor() override;

    Vst3Processor(const Vst3Processor&) = delete;
    Vst3Processor& operator=(const Vst3Processor&) = delete;

    bool load(std::string& error);
    bool open_editor(void* native_parent, int& width, int& height, std::string& error);
    void close_editor() noexcept;

    // Called from the GUI thread while the editor is open. Parameter edits
    // are transferred to the real-time thread using a bounded SPSC queue.
    struct ParameterInfo {
        std::uint32_t id{};
        std::u16string name;
        double normalized_value{};
        std::int32_t step_count{};
        bool read_only{};
    };
    [[nodiscard]] std::vector<ParameterInfo> editor_parameters() const;
    bool set_editor_parameter(std::uint32_t id, double normalized_value);
    bool prepare(const AudioProcessSpec& spec) override;
    void process(float* interleaved_samples, std::size_t frames) noexcept override;
    void reset() noexcept override;
    [[nodiscard]] std::uint32_t latency_frames() const noexcept override;

    [[nodiscard]] const std::string& plugin_path() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vst_host::audio
