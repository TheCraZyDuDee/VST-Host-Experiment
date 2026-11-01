#pragma once

#include "audio_processor.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace vst_host::audio {

// Non-owning ordered processor list. All entries are allocated before audio
// starts; while prepared, entry pointers and their order never change.
// The UI may flip per-entry bypass flags atomically without stopping audio.
class ProcessorChain final {
public:
    bool add(AudioProcessor& processor) {
        if (prepared_) return false;
        processors_.push_back(std::make_unique<Entry>(processor));
        return true;
    }

    bool prepare(const AudioProcessSpec& spec) {
        if (prepared_) return false;
        std::size_t prepared_count = 0;
        for (const auto& entry : processors_) {
            auto* processor = entry->processor;
            if (processor == nullptr || !processor->prepare(spec)) {
                if (processor != nullptr) processor->reset();
                while (prepared_count > 0)
                    processors_[--prepared_count]->processor->reset();
                return false;
            }
            ++prepared_count;
        }
        prepared_ = true;
        return true;
    }

    void process(float* samples, std::size_t frames) noexcept {
        if (!prepared_ || samples == nullptr || frames == 0) return;
        for (const auto& entry : processors_) {
            if (!entry->bypassed.load(std::memory_order_relaxed))
                entry->processor->process(samples, frames);
        }
    }

    // Safe to call from the GUI thread while process() runs on PipeWire.
    // Only a fixed atomic flag changes; no allocations or chain mutation.
    bool set_bypassed(std::size_t index, bool bypassed) noexcept {
        if (index >= processors_.size()) return false;
        processors_[index]->bypassed.store(bypassed, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool is_bypassed(std::size_t index) const noexcept {
        return index < processors_.size() &&
               processors_[index]->bypassed.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        for (const auto& entry : processors_) entry->processor->reset();
    }

    [[nodiscard]] std::uint32_t latency_frames() const noexcept {
        std::uint64_t total = 0;
        for (const auto& entry : processors_) {
            if (!entry->bypassed.load(std::memory_order_relaxed))
                total += entry->processor->latency_frames();
        }
        return static_cast<std::uint32_t>(
            total > std::numeric_limits<std::uint32_t>::max()
                ? std::numeric_limits<std::uint32_t>::max() : total);
    }

    [[nodiscard]] std::size_t size() const noexcept { return processors_.size(); }

private:
    struct Entry final {
        explicit Entry(AudioProcessor& processor_in) noexcept : processor(&processor_in) {}
        AudioProcessor* processor;
        std::atomic<bool> bypassed{false};
    };

    std::vector<std::unique_ptr<Entry>> processors_;
    bool prepared_{false};
};

} // namespace vst_host::audio
