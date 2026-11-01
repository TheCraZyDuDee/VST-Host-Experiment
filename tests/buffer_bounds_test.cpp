#include "audio/buffer_bounds.hpp"

#include <cstddef>
#include <limits>

int main() {
    using vst_host::audio::valid_stereo_float_chunk;
    constexpr std::size_t frame_bytes = 2 * sizeof(float);

    // Standard PipeWire stereo float chunks, including a nonzero byte offset.
    static_assert(valid_stereo_float_chunk(4096, 0, 1024));
    static_assert(valid_stereo_float_chunk(4096, 64, 2048));
    static_assert(valid_stereo_float_chunk(frame_bytes, 0, frame_bytes));
    static_assert(valid_stereo_float_chunk(0, 0, 0));

    // Reject a chunk that would extend outside the mapped allocation.
    static_assert(!valid_stereo_float_chunk(64, 32, 40));
    static_assert(!valid_stereo_float_chunk(64, 65, 0));
    static_assert(!valid_stereo_float_chunk(0, 1, 0));

    // Audio frames must remain aligned; do not interpret partial or
    // misaligned chunks as float samples.
    static_assert(!valid_stereo_float_chunk(64, 2, 16));
    static_assert(!valid_stereo_float_chunk(64, 0, 12));
    static_assert(!valid_stereo_float_chunk(64, 0, 6));

    // Even maliciously large offsets and sizes must not overflow the check.
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    static_assert(!valid_stereo_float_chunk(64, maximum, 8));
    static_assert(!valid_stereo_float_chunk(64, 8, maximum));

    return 0;
}
