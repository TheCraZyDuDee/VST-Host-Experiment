#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace vst_host::audio {

// VST2 effects may declare auxiliary inputs (e.g. ReaComp's sidechain) or
// additional output buses. A stereo microphone host supplies the main stereo
// pair, sends silence to any extra inputs, and uses the first output pair.
// No memory allocation or resizing is permitted once audio starts.
constexpr int max_vst2_channels = 64;

[[nodiscard]] constexpr bool supported_vst2_audio_layout(int inputs, int outputs) noexcept {
    return inputs >= 1 && inputs <= max_vst2_channels &&
           outputs >= 1 && outputs <= max_vst2_channels;
}

class Vst2StereoBuffers final {
public:
    bool prepare(int inputs, int outputs, std::size_t max_frames) {
        if (!supported_vst2_audio_layout(inputs, outputs) || max_frames == 0)
            return false;
        frames_ = max_frames;
        num_inputs_ = inputs;
        num_outputs_ = outputs;
        input_storage_.assign(max_frames * static_cast<std::size_t>(inputs), 0.0F);
        output_storage_.assign(max_frames * static_cast<std::size_t>(outputs), 0.0F);
        input_ptrs_.resize(static_cast<std::size_t>(inputs));
        output_ptrs_.resize(static_cast<std::size_t>(outputs));
        for (int channel = 0; channel < inputs; ++channel)
            input_ptrs_[channel] =
                input_storage_.data() + max_frames * static_cast<std::size_t>(channel);
        for (int channel = 0; channel < outputs; ++channel)
            output_ptrs_[channel] =
                output_storage_.data() + max_frames * static_cast<std::size_t>(channel);
        return true;
    }

    // A single VST2 processReplacing() call uses these full channel arrays.
    void set_input(const float* interleaved, std::size_t offset,
                   std::size_t frames) noexcept {
        if (!interleaved || frames > frames_) return;
        for (std::size_t i = 0; i < frames; ++i) {
            const auto left = interleaved[2 * (offset + i)];
            const auto right = interleaved[2 * (offset + i) + 1];
            input_ptrs_[0][i] = num_inputs_ == 1 ? 0.5F * (left + right) : left;
            if (num_inputs_ > 1) input_ptrs_[1][i] = right;
        }
        for (int channel = 2; channel < num_inputs_; ++channel)
            std::fill_n(input_ptrs_[channel], frames, 0.0F);
        for (int channel = 0; channel < num_outputs_; ++channel)
            std::fill_n(output_ptrs_[channel], frames, 0.0F);
    }

    void copy_output(float* interleaved, std::size_t offset,
                     std::size_t frames) const noexcept {
        if (!interleaved || frames > frames_) return;
        for (std::size_t i = 0; i < frames; ++i) {
            interleaved[2 * (offset + i)] = output_ptrs_[0][i];
            interleaved[2 * (offset + i) + 1] = num_outputs_ == 1
                ? output_ptrs_[0][i] : output_ptrs_[1][i];
        }
    }

    [[nodiscard]] float** inputs() noexcept { return input_ptrs_.data(); }
    [[nodiscard]] float** outputs() noexcept { return output_ptrs_.data(); }
    [[nodiscard]] int input_count() const noexcept { return num_inputs_; }
    [[nodiscard]] int output_count() const noexcept { return num_outputs_; }

private:
    std::size_t frames_{};
    int num_inputs_{};
    int num_outputs_{};
    std::vector<float> input_storage_;
    std::vector<float> output_storage_;
    std::vector<float*> input_ptrs_;
    std::vector<float*> output_ptrs_;
};

} // namespace vst_host::audio
