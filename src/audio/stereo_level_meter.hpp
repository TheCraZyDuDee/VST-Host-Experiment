#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vst_host::audio {

struct MeterChannelReading {
    // Linear amplitude (1.0 = 0 dBFS); peak has a short release, RMS is
    // smoothed so both remain readable at the GUI's 200 ms polling rate.
    float peak{};
    float rms{};
    // Frames with a channel sample at or beyond digital full scale.
    std::uint64_t clipped_frames{};
};

struct StereoMeterReading {
    MeterChannelReading left;
    MeterChannelReading right;
    std::uint64_t observed_frames{};
};

// Each meter is written by exactly one PipeWire callback (capture or output)
// and sampled by the GUI. No allocations, locks, logging, Qt calls, or
// cross-thread floating-point state mutations on the audio path.
// All cross-thread values are independent, relaxed atomic snapshots.
class StereoLevelMeter {
public:
    void observe(const float* samples, std::size_t frames) noexcept {
        if (samples == nullptr || frames == 0) return;

        double left_energy = 0.0;
        double right_energy = 0.0;
        float left_peak = 0.0F;
        float right_peak = 0.0F;
        std::uint64_t left_clips = 0;
        std::uint64_t right_clips = 0;

        for (std::size_t i = 0; i < frames; ++i) {
            // Plugins may return NaN/Inf. Do not alter audio here, but also
            // never let invalid samples poison the meter or GUI values.
            const float left = samples[2 * i];
            const float right = samples[2 * i + 1];
            const float l = std::isfinite(left) ? std::fabs(left) : 0.0F;
            const float r = std::isfinite(right) ? std::fabs(right) : 0.0F;
            left_peak = std::max(left_peak, l);
            right_peak = std::max(right_peak, r);
            left_energy += static_cast<double>(l) * l;
            right_energy += static_cast<double>(r) * r;
            if (l >= 1.0F) ++left_clips;
            if (r >= 1.0F) ++right_clips;
        }

        // Frame-based release rates are independent of the PipeWire quantum:
        // roughly 250 ms peak release and 120 ms RMS smoothing at 48 kHz.
        constexpr double rate = 48000.0;
        const float peak_decay = static_cast<float>(
            std::max(0.0, 1.0 - static_cast<double>(frames) / (rate * 0.25)));
        const double rms_alpha = std::min(
            1.0, static_cast<double>(frames) / (rate * 0.12));
        const double inverse_frames = 1.0 / static_cast<double>(frames);

        update_channel(left_, left_peak, left_energy * inverse_frames,
                       peak_decay, rms_alpha, left_clips);
        update_channel(right_, right_peak, right_energy * inverse_frames,
                       peak_decay, rms_alpha, right_clips);
        observed_frames_.fetch_add(frames, std::memory_order_relaxed);
    }

    [[nodiscard]] StereoMeterReading reading() const noexcept {
        return StereoMeterReading{
            .left = read_channel(left_),
            .right = read_channel(right_),
            .observed_frames = observed_frames_.load(std::memory_order_relaxed),
        };
    }

private:
    struct Channel {
        // Producer-only variables: the GUI never accesses these directly.
        float held_peak{};
        double mean_square{};
        std::atomic<float> published_peak{0.0F};
        std::atomic<float> published_rms{0.0F};
        std::atomic<std::uint64_t> clipped_frames{0};
    };

    static void update_channel(Channel& channel, float peak,
                               double mean_square, float decay,
                               double alpha, std::uint64_t clipped) noexcept {
        channel.held_peak = std::max(peak, channel.held_peak * decay);
        channel.mean_square += (mean_square - channel.mean_square) * alpha;
        channel.published_peak.store(channel.held_peak, std::memory_order_relaxed);
        channel.published_rms.store(
            static_cast<float>(std::sqrt(std::max(0.0, channel.mean_square))),
            std::memory_order_relaxed);
        if (clipped > 0)
            channel.clipped_frames.fetch_add(clipped, std::memory_order_relaxed);
    }

    [[nodiscard]] static MeterChannelReading read_channel(
        const Channel& channel) noexcept {
        return MeterChannelReading{
            .peak = channel.published_peak.load(std::memory_order_relaxed),
            .rms = channel.published_rms.load(std::memory_order_relaxed),
            .clipped_frames = channel.clipped_frames.load(std::memory_order_relaxed),
        };
    }

    Channel left_;
    Channel right_;
    std::atomic<std::uint64_t> observed_frames_{0};
};

} // namespace vst_host::audio
