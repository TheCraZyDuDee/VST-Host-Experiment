#include "audio/gain_processor.hpp"

#include <cmath>

int main() {
    using namespace vst_host::audio;

    GainProcessor processor(2.0F);
    if (!processor.prepare(AudioProcessSpec{48000, 2, 128})) {
        return 1;
    }

    float samples[] = {0.25F, -0.25F, 0.5F, -0.5F};
    processor.process(samples, 2);
    if (samples[0] != 0.5F || samples[1] != -0.5F ||
        samples[2] != 1.0F || samples[3] != -1.0F) {
        return 2;
    }

    processor.set_bypassed(true);
    float unchanged[] = {0.25F, -0.25F};
    processor.process(unchanged, 1);
    if (unchanged[0] != 0.25F || unchanged[1] != -0.25F) {
        return 3;
    }

    processor.set_bypassed(false);
    processor.set_gain(8.0F);
    if (processor.gain() != 4.0F) {
        return 4;
    }

    processor.set_gain(std::nanf(""));
    if (processor.gain() != 4.0F) {
        return 5;
    }

    if (processor.prepare(AudioProcessSpec{0, 2, 128})) {
        return 6;
    }

    return 0;
}
