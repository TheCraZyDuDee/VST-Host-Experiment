#pragma once

#include "audio_processor.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace vst_host::audio {

// Non-owning ordered processor list. The owner must keep processors alive for
// the entire prepared/processing lifetime. All storage is built before audio
// starts; process() performs only bounded pointer iteration.
class ProcessorChain final {
public:
    bool add(AudioProcessor& processor) {
        if (prepared_) return false;
        processors_.push_back(&processor);
        return true;
    }

    bool prepare(const AudioProcessSpec& spec) {
        if (prepared_) return false;
        std::size_t prepared_count = 0;
        for (auto* processor : processors_) {
            if (processor == nullptr || !processor->prepare(spec)) {
                if (processor != nullptr) processor->reset();
                while (prepared_count > 0) processors_[--prepared_count]->reset();
                return false;
            }
            ++prepared_count;
        }
        prepared_ = true;
        return true;
    }

    void process(float* samples, std::size_t frames) noexcept {
        if (!prepared_ || samples == nullptr || frames == 0) return;
        for (auto* processor : processors_) processor->process(samples, frames);
    }

    void reset() noexcept {
        for (auto* processor : processors_) processor->reset();
    }

    [[nodiscard]] std::uint32_t latency_frames() const noexcept {
        std::uint64_t total = 0;
        for (const auto* processor : processors_) total += processor->latency_frames();
        return static_cast<std::uint32_t>(
            total > std::numeric_limits<std::uint32_t>::max()
                ? std::numeric_limits<std::uint32_t>::max() : total);
    }

    [[nodiscard]] std::size_t size() const noexcept { return processors_.size(); }

private:
    std::vector<AudioProcessor*> processors_;
    bool prepared_{false};
};

} // namespace vst_host::audio
