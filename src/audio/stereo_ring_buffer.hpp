#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vst_host::audio {

// Single-producer/single-consumer ring buffer. Both callbacks are lock-free
// and perform no dynamic allocation after construction.
class StereoRingBuffer {
public:
    static constexpr std::size_t capacity_frames = 32768;

    struct Frame {
        float left{};
        float right{};
    };

    void push_interleaved(const float* samples, std::size_t frames) noexcept {
        const auto write = write_index_.load(std::memory_order_relaxed);
        const auto read = read_index_.load(std::memory_order_acquire);
        const auto available = capacity_frames - static_cast<std::size_t>(write - read);
        const auto count = std::min(frames, available);

        for (std::size_t i = 0; i < count; ++i) {
            frames_[(write + i) % capacity_frames] = Frame{
                .left = samples[i * 2],
                .right = samples[i * 2 + 1],
            };
        }

        write_index_.store(write + count, std::memory_order_release);
        if (count < frames) {
            dropped_frames_.fetch_add(frames - count, std::memory_order_relaxed);
        }
    }

    // Reads interleaved stereo samples, filling any underrun with silence.
    void pop_interleaved(float* samples, std::size_t frames,
                         std::size_t max_backlog_frames = 512) noexcept {
        auto read = read_index_.load(std::memory_order_relaxed);
        const auto write = write_index_.load(std::memory_order_acquire);
        auto available = static_cast<std::size_t>(write - read);

        // Discard stale audio if the consumer was suspended or fell behind.
        // Keep a small cushion for scheduling jitter instead of replaying a long queue.
        if (available > frames && available - frames > max_backlog_frames) {
            const auto discard = available - frames - max_backlog_frames;
            read += discard;
            read_index_.store(read, std::memory_order_release);
            stale_frames_.fetch_add(discard, std::memory_order_relaxed);
            available -= discard;
        }

        const auto count = std::min(frames, available);

        for (std::size_t i = 0; i < count; ++i) {
            const auto& frame = frames_[(read + i) % capacity_frames];
            samples[i * 2] = frame.left;
            samples[i * 2 + 1] = frame.right;
        }
        std::fill(samples + count * 2, samples + frames * 2, 0.0F);

        read_index_.store(read + count, std::memory_order_release);
        if (count < frames) {
            underrun_frames_.fetch_add(frames - count, std::memory_order_relaxed);
        }
    }

    // Reads up to the requested number of frames, filling any underrun with silence.
    void pop_planar(float* left, float* right, std::size_t frames) noexcept {
        const auto read = read_index_.load(std::memory_order_relaxed);
        const auto write = write_index_.load(std::memory_order_acquire);
        const auto count = std::min(frames, static_cast<std::size_t>(write - read));

        for (std::size_t i = 0; i < count; ++i) {
            const auto& frame = frames_[(read + i) % capacity_frames];
            left[i] = frame.left;
            right[i] = frame.right;
        }
        std::fill(left + count, left + frames, 0.0F);
        std::fill(right + count, right + frames, 0.0F);

        read_index_.store(read + count, std::memory_order_release);
        if (count < frames) {
            underrun_frames_.fetch_add(frames - count, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::uint64_t dropped_frames() const noexcept {
        return dropped_frames_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t stale_frames() const noexcept {
        return stale_frames_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t underrun_frames() const noexcept {
        return underrun_frames_.load(std::memory_order_relaxed);
    }

private:
    std::array<Frame, capacity_frames> frames_{};
    alignas(64) std::atomic<std::uint64_t> write_index_{0};
    alignas(64) std::atomic<std::uint64_t> read_index_{0};
    std::atomic<std::uint64_t> dropped_frames_{0};
    std::atomic<std::uint64_t> stale_frames_{0};
    std::atomic<std::uint64_t> underrun_frames_{0};
};

} // namespace vst_host::audio
