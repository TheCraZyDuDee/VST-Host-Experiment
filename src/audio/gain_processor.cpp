#include "gain_processor.hpp"

#include <algorithm>
#include <cmath>

namespace vst_host::audio {

GainProcessor::GainProcessor(float gain, bool bypassed) noexcept
    : gain_(std::isfinite(gain) ? std::clamp(gain, 0.0F, 4.0F) : 1.0F),
      bypassed_(bypassed) {}

bool GainProcessor::prepare(const AudioProcessSpec& spec) {
    if (spec.sample_rate == 0 || spec.channels == 0 || spec.max_block_size == 0) {
        return false;
    }
    channels_ = spec.channels;
    return true;
}

void GainProcessor::process(float* samples, std::size_t frames) noexcept {
    if (bypassed_ || gain_ == 1.0F || samples == nullptr) {
        return;
    }
    const auto sample_count = frames * static_cast<std::size_t>(channels_);
    for (std::size_t i = 0; i < sample_count; ++i) {
        samples[i] *= gain_;
    }
}

void GainProcessor::reset() noexcept {}

void GainProcessor::set_gain(float gain) noexcept {
    if (std::isfinite(gain)) {
        gain_ = std::clamp(gain, 0.0F, 4.0F);
    }
}

void GainProcessor::set_bypassed(bool bypassed) noexcept {
    bypassed_ = bypassed;
}

float GainProcessor::gain() const noexcept {
    return gain_;
}

bool GainProcessor::bypassed() const noexcept {
    return bypassed_;
}

} // namespace vst_host::audio
