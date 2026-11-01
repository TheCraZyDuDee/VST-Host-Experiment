#include "audio/vst2_audio_layout.hpp"

#include <cmath>
#include <cstddef>

int main() {
    using namespace vst_host::audio;
    if (!supported_vst2_audio_layout(4, 2) ||
        !supported_vst2_audio_layout(2, 4) ||
        !supported_vst2_audio_layout(1, 1) ||
        supported_vst2_audio_layout(0, 2) ||
        supported_vst2_audio_layout(2, 0) ||
        supported_vst2_audio_layout(128, 2))
        return 1;

    Vst2StereoBuffers stereo_sidechain;
    if (!stereo_sidechain.prepare(4, 2, 8) ||
        stereo_sidechain.input_count() != 4 ||
        stereo_sidechain.output_count() != 2)
        return 2;

    float audio[]{0.2F, -0.4F, 0.7F, 0.1F};
    stereo_sidechain.set_input(audio, 0, 2);
    float** inputs = stereo_sidechain.inputs();
    float** outputs = stereo_sidechain.outputs();
    if (inputs[0][0] != 0.2F || inputs[1][0] != -0.4F ||
        inputs[0][1] != 0.7F || inputs[1][1] != 0.1F)
        return 3;
    if (inputs[2][0] != 0 || inputs[3][0] != 0 ||
        inputs[2][1] != 0 || inputs[3][1] != 0)
        return 4;

    // Exercise the plugin's main output; any unwritten channels must start
    // silent rather than exposing stale scratch memory from prior blocks.
    outputs[0][0] = 0.3F;
    outputs[1][0] = -0.3F;
    outputs[0][1] = 0.6F;
    outputs[1][1] = 0.9F;
    stereo_sidechain.copy_output(audio, 0, 2);
    if (audio[0] != 0.3F || audio[1] != -0.3F ||
        audio[2] != 0.6F || audio[3] != 0.9F)
        return 5;

    // Poison auxiliary and output buffers to ensure every block clears them.
    inputs[2][0] = 13.0F;
    outputs[0][0] = 14.0F;
    stereo_sidechain.set_input(audio, 0, 2);
    if (inputs[2][0] != 0.0F || outputs[0][0] != 0.0F) return 6;

    Vst2StereoBuffers mono;
    if (!mono.prepare(1, 1, 4)) return 7;
    float mono_audio[]{0.8F, -0.2F};
    mono.set_input(mono_audio, 0, 1);
    if (std::abs(mono.inputs()[0][0] - 0.3F) > 0.0001F) return 8;
    mono.outputs()[0][0] = 0.25F;
    mono.copy_output(mono_audio, 0, 1);
    if (mono_audio[0] != 0.25F || mono_audio[1] != 0.25F) return 9;

    Vst2StereoBuffers multi_out;
    if (!multi_out.prepare(2, 4, 4)) return 10;
    float multi_audio[]{0.1F, 0.2F};
    multi_out.set_input(multi_audio, 0, 1);
    multi_out.outputs()[0][0] = -0.2F;
    multi_out.outputs()[1][0] = 0.5F;
    multi_out.outputs()[2][0] = 4.0F; // auxiliary bus is not mixed into mic
    multi_out.copy_output(multi_audio, 0, 1);
    if (multi_audio[0] != -0.2F || multi_audio[1] != 0.5F) return 11;

    return 0;
}
