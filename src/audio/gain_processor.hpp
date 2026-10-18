#pragma once

#include "audio_processor.hpp"

namespace vst_host::audio {

class GainProcessor final : public AudioProcessor {
public:
    explicit GainProcessor(float gain = 1.0F, bool bypassed = false) noexcept;

    bool prepare(const AudioProcessSpec& spec) override;
    void process(float* interleaved_samples, std::size_t frames) noexcept override;
    void reset() noexcept override;

    void set_gain(float gain) noexcept;
    void set_bypassed(bool bypassed) noexcept;
    [[nodiscard]] float gain() const noexcept;
    [[nodiscard]] bool bypassed() const noexcept;

private:
    float gain_{1.0F};
    bool bypassed_{false};
    std::uint32_t channels_{2};
};

} // namespace vst_host::audio
