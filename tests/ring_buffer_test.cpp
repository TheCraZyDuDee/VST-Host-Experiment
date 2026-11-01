#include "audio/stereo_ring_buffer.hpp"

#include <vector>

int main() {
    using vst_host::audio::StereoRingBuffer;

    StereoRingBuffer buffer;
    const float input[] = {0.25F, -0.25F, 0.5F, -0.5F, 0.75F, -0.75F};
    buffer.push_interleaved(input, 3);

    float left[4]{};
    float right[4]{};
    buffer.pop_planar(left, right, 4);

    if (left[0] != 0.25F || right[0] != -0.25F ||
        left[1] != 0.5F || right[1] != -0.5F ||
        left[2] != 0.75F || right[2] != -0.75F ||
        left[3] != 0.0F || right[3] != 0.0F ||
        buffer.underrun_frames() != 1) {
        return 1;
    }

    StereoRingBuffer full;
    std::vector<float> oversized((StereoRingBuffer::capacity_frames + 1) * 2, 0.125F);
    full.push_interleaved(oversized.data(), StereoRingBuffer::capacity_frames + 1);
    if (full.dropped_frames() != 1) {
        return 2;
    }

    StereoRingBuffer interleaved;
    const float stereo_input[] = {0.1F, -0.1F, 0.2F, -0.2F};
    interleaved.push_interleaved(stereo_input, 2);
    float stereo_output[6]{};
    interleaved.pop_interleaved(stereo_output, 3);
    if (stereo_output[0] != 0.1F || stereo_output[1] != -0.1F ||
        stereo_output[2] != 0.2F || stereo_output[3] != -0.2F ||
        stereo_output[4] != 0.0F || stereo_output[5] != 0.0F ||
        interleaved.underrun_frames() != 1) {
        return 3;
    }

    StereoRingBuffer backlog;
    const float backlog_input[] = {
        1.0F, -1.0F, 2.0F, -2.0F, 3.0F, -3.0F,
        4.0F, -4.0F, 5.0F, -5.0F, 6.0F, -6.0F
    };
    backlog.push_interleaved(backlog_input, 6);
    float recent_output[4]{};
    backlog.pop_interleaved(recent_output, 2, 1);
    if (recent_output[0] != 4.0F || recent_output[1] != -4.0F ||
        recent_output[2] != 5.0F || recent_output[3] != -5.0F ||
        backlog.stale_frames() != 3 || backlog.dropped_frames() != 0) {
        return 4;
    }

    // Stale queue trimming is not the same as a producer overflow.
    if (full.stale_frames() != 0 || buffer.dropped_frames() != 0) return 5;


    // Exercise the actual circular-index wrap, not just a short queue.
    // Fill almost the entire capacity, drain, then cross the array boundary
    // with stereo samples of different signs to detect swapped channels.
    StereoRingBuffer wrapped;
    constexpr auto nearly_full = StereoRingBuffer::capacity_frames - 2;
    std::vector<float> before_wrap(nearly_full * 2);
    for (std::size_t i = 0; i < nearly_full; ++i) {
        before_wrap[2 * i] = static_cast<float>(i + 1);
        before_wrap[2 * i + 1] = -static_cast<float>(i + 1);
    }
    wrapped.push_interleaved(before_wrap.data(), nearly_full);
    std::vector<float> drained(before_wrap.size());
    wrapped.pop_interleaved(drained.data(), nearly_full);
    if (drained != before_wrap || wrapped.underrun_frames() != 0 ||
        wrapped.stale_frames() != 0) return 6;

    const float crossing[] = {7.0F, -7.0F, 8.0F, -8.0F,
                              9.0F, -9.0F, 10.0F, -10.0F};
    wrapped.push_interleaved(crossing, 4);
    float after_wrap[8]{};
    wrapped.pop_interleaved(after_wrap, 4);
    for (std::size_t i = 0; i < 8; ++i)
        if (after_wrap[i] != crossing[i]) return 7;
    if (wrapped.dropped_frames() != 0 ||
        wrapped.stale_frames() != 0 ||
        wrapped.underrun_frames() != 0) return 8;

    // After a pinned microphone disconnect, old queued audio must not be
    // replayed. Only fresh samples should survive a consumer-side discard.
    StereoRingBuffer reconnect;
    reconnect.push_interleaved(input, 3);
    reconnect.discard_pending();
    const float reconnected_input[] = {0.9F, -0.9F, 0.8F, -0.8F};
    reconnect.push_interleaved(reconnected_input, 2);
    float reconnected_output[6]{};
    reconnect.pop_interleaved(reconnected_output, 3);
    if (reconnected_output[0] != 0.9F || reconnected_output[1] != -0.9F ||
        reconnected_output[2] != 0.8F || reconnected_output[3] != -0.8F ||
        reconnected_output[4] != 0.0F || reconnected_output[5] != 0.0F ||
        reconnect.underrun_frames() != 1 ||
        reconnect.dropped_frames() != 0 || reconnect.stale_frames() != 0)
        return 9;

    return 0;
}
