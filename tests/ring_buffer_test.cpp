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
    return 0;
}
