#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vst_host::audio {

// Single producer (plugin editor / UI thread), single consumer (audio callback).
// No locks, allocation, logging, or plugin SDK calls are performed in this queue.
// A full queue rejects the newest event rather than blocking the audio thread.
class ParameterEventQueue final {
public:
    struct Event {
        std::uint32_t id{};
        double normalized_value{};
    };
    static constexpr std::size_t capacity = 4096;

    [[nodiscard]] bool push(std::uint32_t id, double value) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) >= capacity) {
            return false;
        }
        events_[write % capacity] = Event{id, value};
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(Event& event) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        event = events_[read % capacity];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<Event, capacity> events_{};
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
};

} // namespace vst_host::audio
