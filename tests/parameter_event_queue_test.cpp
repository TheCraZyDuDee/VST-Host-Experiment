#include "audio/parameter_event_queue.hpp"

#include <cmath>
#include <cstddef>

int main() {
    using vst_host::audio::ParameterEventQueue;
    ParameterEventQueue queue;
    ParameterEventQueue::Event event{};
    if (queue.pop(event)) return 1;
    if (!queue.push(3, 0.25) || !queue.push(9, 0.75)) return 2;
    if (!queue.pop(event) || event.id != 3 || event.normalized_value != 0.25) return 3;
    if (!queue.pop(event) || event.id != 9 || event.normalized_value != 0.75) return 4;
    if (queue.pop(event)) return 5;

    for (std::size_t i = 0; i < ParameterEventQueue::capacity; ++i)
        if (!queue.push(static_cast<std::uint32_t>(i), static_cast<double>(i))) return 6;
    if (queue.push(10000, 1.0)) return 7;
    for (std::size_t i = 0; i < ParameterEventQueue::capacity; ++i) {
        if (!queue.pop(event) || event.id != i || event.normalized_value != i) return 8;
    }
    if (queue.pop(event)) return 9;

    // Wrap around both indices, preserving the FIFO ordering.
    for (std::size_t i = 0; i < 100; ++i)
        if (!queue.push(static_cast<std::uint32_t>(i + 50), 0.5)) return 10;
    for (std::size_t i = 0; i < 100; ++i)
        if (!queue.pop(event) || event.id != i + 50) return 11;
    return queue.pop(event) ? 12 : 0;
}
