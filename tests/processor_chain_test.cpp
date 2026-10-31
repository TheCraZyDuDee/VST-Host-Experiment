#include "audio/processor_chain.hpp"

#include <cstddef>
#include <cstdint>

namespace {
class Multiply final : public vst_host::audio::AudioProcessor {
public:
    explicit Multiply(float factor, std::uint32_t latency = 0) : factor_(factor), latency_(latency) {}
    bool prepare(const vst_host::audio::AudioProcessSpec& spec) override {
        return spec.sample_rate == 48000 && spec.channels == 2 && spec.max_block_size > 0;
    }
    void process(float* samples, std::size_t frames) noexcept override {
        for (std::size_t i = 0; i < frames * 2; ++i) samples[i] *= factor_;
    }
    void reset() noexcept override { ++resets_; }
    std::uint32_t latency_frames() const noexcept override { return latency_; }
    int resets() const noexcept { return resets_; }
private:
    float factor_;
    std::uint32_t latency_;
    int resets_{0};
};
}

int main() {
    using namespace vst_host::audio;
    Multiply first(2.0F, 32);
    Multiply second(0.5F, 64);
    ProcessorChain chain;
    if (!chain.add(first) || !chain.add(second) || chain.size() != 2) return 1;
    if (!chain.prepare(AudioProcessSpec{48000, 2, 128})) return 2;
    if (chain.add(first)) return 3;
    float samples[]{0.25F, -0.25F, 0.5F, -0.5F};
    chain.process(samples, 2);
    if (samples[0] != 0.25F || samples[1] != -0.25F ||
        samples[2] != 0.5F || samples[3] != -0.5F) return 4;
    if (chain.latency_frames() != 96) return 5;
    chain.reset();
    if (first.resets() != 1 || second.resets() != 1) return 6;
    return 0;
}
