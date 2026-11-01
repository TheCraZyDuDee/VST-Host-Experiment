#pragma once

#include <chrono>
#include <cmath>
#include <ctime>

namespace vst_host::gui {

// A percentage relative to one logical CPU core: 100% means one core fully
// occupied and may legitimately be exceeded by a multithreaded host process.
// Invalid/wrapped or nonmonotonic samples are ignored rather than printed as
// nonsense or interpreted as a negative CPU load.
[[nodiscard]] inline double process_cpu_percent(double cpu_delta_seconds,
                                                 double wall_delta_seconds) noexcept {
    if (!std::isfinite(cpu_delta_seconds) ||
        !std::isfinite(wall_delta_seconds) ||
        cpu_delta_seconds < 0.0 || wall_delta_seconds <= 0.0)
        return 0.0;
    const double percentage = 100.0 * cpu_delta_seconds / wall_delta_seconds;
    return std::isfinite(percentage) ? percentage : 0.0;
}

// Called only from the Qt GUI thread. std::clock() measures the CPU time of
// THIS host process, including its threads, but not external Wine bridge
// processes. No timing or syscalls are added to the PipeWire callback path.
class ProcessCpuMeter {
public:
    void reset() noexcept { initialized_ = false; }

    [[nodiscard]] double sample() noexcept {
        const auto wall = std::chrono::steady_clock::now();
        const auto ticks = std::clock();
        if (ticks == static_cast<std::clock_t>(-1)) {
            initialized_ = false;
            return 0.0;
        }
        const auto previous_wall = previous_wall_;
        const auto previous_ticks = previous_ticks_;
        previous_wall_ = wall;
        previous_ticks_ = ticks;
        if (!initialized_) {
            initialized_ = true;
            return 0.0;
        }
        if (ticks < previous_ticks) return 0.0; // clock_t rollover
        const auto wall_seconds =
            std::chrono::duration<double>(wall - previous_wall).count();
        const double cpu_seconds = static_cast<double>(ticks - previous_ticks) /
                                    static_cast<double>(CLOCKS_PER_SEC);
        return process_cpu_percent(cpu_seconds, wall_seconds);
    }

private:
    bool initialized_{false};
    std::chrono::steady_clock::time_point previous_wall_{};
    std::clock_t previous_ticks_{};
};

} // namespace vst_host::gui
