#pragma once

#include <cstddef>
#include <cstdint>

namespace vst_host::audio {

struct AudioProcessSpec {
    std::uint32_t sample_rate{48000};
    std::uint32_t channels{2};
    std::uint32_t max_block_size{1024};
};

// Implementations must allocate/configure in prepare(), never in process().
// process() runs on the real-time audio callback and must not block, log,
// perform I/O, or throw exceptions.
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    virtual bool prepare(const AudioProcessSpec& spec) = 0;
    virtual void process(float* interleaved_samples, std::size_t frames) noexcept = 0;
    virtual void reset() noexcept = 0;
    [[nodiscard]] virtual std::uint32_t latency_frames() const noexcept { return 0; }
};

} // namespace vst_host::audio
