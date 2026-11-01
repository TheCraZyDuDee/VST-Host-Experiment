#pragma once

#include "stereo_ring_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace vst_host::audio {

// An empty name is the user's explicit choice to follow PipeWire's default.
// A nonempty, stable node.name must never be silently replaced with a
// different microphone while the requested device is disconnected.
struct CaptureTargetPolicy {
    bool pinned{};
    bool dont_fallback{};
    bool linger{};
    bool dont_move{};
};

[[nodiscard]] constexpr CaptureTargetPolicy capture_target_policy(
    std::string_view selected_node_name) noexcept {
    const bool pinned = !selected_node_name.empty();
    return {
        .pinned = pinned,
        .dont_fallback = pinned,
        .linger = pinned,
        .dont_move = pinned,
    };
}

// The capture and virtual microphone use separate PipeWire streams. Keep the
// virtual microphone publishing silence while the selected source is absent.
// Discard any queued audio on the consumer side, so nothing recorded before
// disconnection is unexpectedly replayed after reconnection.
inline void mute_disconnected_capture(StereoRingBuffer& ring,
                                       float* output, std::size_t frames) noexcept {
    ring.discard_pending();
    if (output && frames > 0) std::fill_n(output, frames * 2, 0.0F);
}

} // namespace vst_host::audio
