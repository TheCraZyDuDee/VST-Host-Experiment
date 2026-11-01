#include "audio/capture_target_policy.hpp"

#include <cstddef>

int main() {
    using namespace vst_host::audio;

    const auto follows_default = capture_target_policy("");
    if (follows_default.pinned || follows_default.dont_fallback ||
        follows_default.linger || follows_default.dont_move) return 1;

    const auto pinned = capture_target_policy("alsa_input.usb-Microphone-00.mono-fallback");
    if (!pinned.pinned || !pinned.dont_fallback ||
        !pinned.linger || !pinned.dont_move) return 2;

    StereoRingBuffer ring;
    const float old_audio[]{0.25F, -0.25F, 0.5F, -0.5F};
    ring.push_interleaved(old_audio, 2);
    float output[]{2.0F, 2.0F, 2.0F, 2.0F};
    mute_disconnected_capture(ring, output, 2);
    for (const float sample : output) if (sample != 0.0F) return 3;

    // The first frame after reconnecting must be the NEW microphone's audio,
    // not stale audio buffered before unplugging it.
    const float new_audio[]{0.75F, -0.75F};
    ring.push_interleaved(new_audio, 1);
    float stereo[2]{};
    ring.pop_interleaved(stereo, 1);
    if (stereo[0] != new_audio[0] || stereo[1] != new_audio[1]) return 4;
    if (ring.underrun_frames() != 0) return 5;
    return 0;
}
