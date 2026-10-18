#include "audio/stereo_ring_buffer.hpp"

#include <cassert>
#include <vector>

int main() {
    using vst_host::audio::StereoRingBuffer;

    StereoRingBuffer buffer;
    const float input[] = {0.25F, -0.25F, 0.5F, -0.5F, 0.75F, -0.75F};
    buffer.push_interleaved(input, 3);

    float left[4]{};
    float right[4]{};
    buffer.pop_planar(left, right, 4);

    assert(left[0] == 0.25F && right[0] == -0.25F);
    assert(left[1] == 0.5F && right[1] == -0.5F);
    assert(left[2] == 0.75F && right[2] == -0.75F);
    assert(left[3] == 0.0F && right[3] == 0.0F);
    assert(buffer.underrun_frames() == 1);

    StereoRingBuffer full;
    std::vector<float> oversized((StereoRingBuffer::capacity_frames + 1) * 2, 0.125F);
    full.push_interleaved(oversized.data(), StereoRingBuffer::capacity_frames + 1);
    assert(full.dropped_frames() == 1);
    return 0;
}
