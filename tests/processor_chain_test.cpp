#include "audio/processor_chain.hpp"

#include <cstddef>
#include <cstdint>

namespace {
class Affine final : public vst_host::audio::AudioProcessor {
public:
    Affine(float scale, float offset, std::uint32_t latency)
        : scale_(scale), offset_(offset), latency_(latency) {}

    bool prepare(const vst_host::audio::AudioProcessSpec& spec) override {
        return spec.sample_rate == 48000 && spec.channels == 2 && spec.max_block_size > 0;
    }
    void process(float* samples, std::size_t frames) noexcept override {
        for (std::size_t i = 0; i < frames * 2; ++i)
            samples[i] = samples[i] * scale_ + offset_;
    }
    void reset() noexcept override { ++resets_; }
    std::uint32_t latency_frames() const noexcept override { return latency_; }
    int resets() const noexcept { return resets_; }

private:
    float scale_;
    float offset_;
    std::uint32_t latency_;
    int resets_{0};
};
}

int main() {
    using namespace vst_host::audio;
    Affine first(2.0F, 0.0F, 32);
    Affine second(1.0F, 1.0F, 64);
    ProcessorChain chain;
    if (!chain.add(first) || !chain.add(second) || chain.size() != 2) return 1;
    if (!chain.prepare(AudioProcessSpec{48000, 2, 128})) return 2;
    if (chain.add(first)) return 3;

    float samples[]{0.25F, -0.25F, 0.5F, -0.5F};
    chain.process(samples, 2);
    if (samples[0] != 1.5F || samples[1] != 0.5F ||
        samples[2] != 2.0F || samples[3] != 0.0F) return 4;
    if (chain.latency_frames() != 96) return 5;

    // A running chain can bypass either effect independently, without
    // reallocating or reordering its prepared processor entries.
    if (!chain.set_bypassed(1, true) || !chain.is_bypassed(1)) return 7;
    if (chain.latency_frames() != 32) return 8;
    float bypass_second[]{0.25F, -0.25F, 0.5F, -0.5F};
    chain.process(bypass_second, 2);
    if (bypass_second[0] != 0.5F || bypass_second[1] != -0.5F ||
        bypass_second[2] != 1.0F || bypass_second[3] != -1.0F) return 9;

    if (!chain.set_bypassed(0, true)) return 10;
    if (chain.latency_frames() != 0) return 11;
    float bypass_all[]{0.25F, -0.25F};
    chain.process(bypass_all, 1);
    if (bypass_all[0] != 0.25F || bypass_all[1] != -0.25F) return 12;

    if (!chain.set_bypassed(0, false) || !chain.set_bypassed(1, false) ||
        chain.is_bypassed(0) || chain.is_bypassed(1)) return 13;
    if (chain.latency_frames() != 96) return 14;
    if (chain.set_bypassed(2, true)) return 15;

    chain.reset();
    if (first.resets() != 1 || second.resets() != 1) return 6;
    return 0;
}
