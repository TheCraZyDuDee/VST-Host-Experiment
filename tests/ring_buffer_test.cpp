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

    return 0;
}
