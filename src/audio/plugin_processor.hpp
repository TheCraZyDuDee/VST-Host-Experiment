#pragma once

#include "audio_processor.hpp"
#include "plugin_state.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vst_host::audio {

// The audio engine and Qt editor operate on a common interface. Native VST3,
// native VST2 and Wine bridge modules all participate in the same chain.
class PluginProcessor : public AudioProcessor {
public:
    ~PluginProcessor() override = default;
    struct ParameterInfo {
        std::uint32_t id{};
        std::u16string name;
        double normalized_value{};
        std::int32_t step_count{};
        bool read_only{};
    };
    virtual bool load(std::string& error) = 0;
    virtual bool restore_plugin_state(const PluginState&, std::string& error) = 0;
    virtual bool snapshot_plugin_state(PluginState&, std::string& error) = 0;
    virtual bool open_editor(void* native_parent, int& width, int& height,
                             std::string& error) = 0;
    virtual void close_editor() noexcept = 0;
    [[nodiscard]] virtual std::vector<ParameterInfo> editor_parameters() const = 0;
    virtual bool set_editor_parameter(std::uint32_t id, double normalized_value) = 0;
    [[nodiscard]] virtual const std::string& plugin_path() const noexcept = 0;
};

} // namespace vst_host::audio
