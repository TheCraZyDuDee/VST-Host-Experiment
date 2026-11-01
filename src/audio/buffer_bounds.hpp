#pragma once

#include <cstddef>

namespace vst_host::audio {

// PipeWire's spa_chunk offset and size are in bytes relative to spa_data::data.
// Reject invalid ranges before turning them into float pointers. Subtraction
// after the offset check avoids an overflow when the provided values are large.
[[nodiscard]] constexpr bool valid_stereo_float_chunk(
    std::size_t max_bytes, std::size_t offset_bytes,
    std::size_t chunk_bytes) noexcept {
    constexpr auto frame_bytes = 2 * sizeof(float);
    return offset_bytes <= max_bytes &&
           chunk_bytes <= max_bytes - offset_bytes &&
           offset_bytes % alignof(float) == 0 &&
           chunk_bytes % frame_bytes == 0;
}

} // namespace vst_host::audio
