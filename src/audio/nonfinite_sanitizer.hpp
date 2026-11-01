#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vst_host::audio {

// Replace only invalid floating-point samples with silence before feeding
// third-party DSP, and again before publishing the virtual microphone.
// Finite samples, including over-full-scale peaks, are left unchanged:
// this is not a limiter or normalization stage. No allocations or locks.
[[nodiscard]] inline std::uint64_t sanitize_nonfinite_stereo(
    float* samples, std::size_t frames) noexcept {
    if (samples == nullptr) return 0;
    std::uint64_t replaced = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        auto& left = samples[frame * 2];
        auto& right = samples[frame * 2 + 1];
        if (!std::isfinite(left)) {
            left = 0.0F;
            ++replaced;
        }
        if (!std::isfinite(right)) {
            right = 0.0F;
            ++replaced;
        }
    }
    return replaced;
}

} // namespace vst_host::audio
