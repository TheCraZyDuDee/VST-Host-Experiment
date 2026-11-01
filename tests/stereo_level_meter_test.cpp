#include "audio/stereo_level_meter.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace {
bool near(float actual, float expected, float tolerance = 0.002F) {
    return std::fabs(actual - expected) <= tolerance;
}
}

int main() {
    using vst_host::audio::StereoLevelMeter;

    StereoLevelMeter meter;
    assert(meter.reading().observed_frames == 0);
    meter.observe(nullptr, 12);
    meter.observe(nullptr, 0);
    assert(meter.reading().observed_frames == 0);

    // Constant stereo signal: a full smoothing window yields exact RMS
    // while the peak retains separate L/R channel information.
    std::vector<float> signal(48000 * 2);
    for (std::size_t i = 0; i < 48000; ++i) {
        signal[2 * i] = 0.5F;
        signal[2 * i + 1] = -0.25F;
    }
    meter.observe(signal.data(), 48000);
    auto result = meter.reading();
    assert(result.observed_frames == 48000);
    assert(near(result.left.peak, 0.5F) && near(result.left.rms, 0.5F));
    assert(near(result.right.peak, 0.25F) && near(result.right.rms, 0.25F));
    assert(result.left.clipped_frames == 0 && result.right.clipped_frames == 0);

    // Positive and negative full-scale samples count as clipping per channel.
    const float clipping[] = {1.0F, -1.25F, 0.4F, -0.7F,
                              -2.0F, 0.0F, 0.5F, 1.0F};
    meter.observe(clipping, 4);
    result = meter.reading();
    assert(result.left.clipped_frames == 2);
    assert(result.right.clipped_frames == 2);
    assert(result.left.peak >= 2.0F && result.right.peak >= 1.25F);
    assert(result.observed_frames == 48004);

    // After silence, the displayed peaks and RMS should decay, not freeze.
    const auto old_left = result.left.peak;
    const auto old_rms = result.left.rms;
    const std::vector<float> silence(48000 * 2);
    meter.observe(silence.data(), 48000);
    result = meter.reading();
    assert(result.left.peak < old_left && result.left.rms < old_rms);
    assert(result.left.clipped_frames == 2 && result.right.clipped_frames == 2);

    // Broken plug-ins can emit nonfinite values; they are ignored by the
    // observer without changing the original audio or poisoning the meter.
    StereoLevelMeter invalid;
    const float bad[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), 0.5F,
    };
    invalid.observe(bad, 2);
    result = invalid.reading();
    assert(std::isfinite(result.left.peak) && std::isfinite(result.left.rms));
    assert(std::isfinite(result.right.peak) && std::isfinite(result.right.rms));
    assert(result.left.peak == 0.0F && result.left.clipped_frames == 0);
    assert(near(result.right.peak, 0.5F));
    assert(result.right.clipped_frames == 0);

    return 0;
}
