#include "audio/vst3_processor.hpp"

#include <cassert>
#include <string>

int main() {
    vst_host::audio::Vst3Processor processor("unused.vst3");
    std::string error;
    assert(!processor.load(error));
    assert(!error.empty());

    const vst_host::audio::AudioProcessSpec spec{48000, 2, 128};
    assert(!processor.prepare(spec));
    assert(processor.latency_frames() == 0);

    float samples[] = {0.25F, -0.5F, 0.75F, -1.0F};
    processor.process(samples, 2);
    assert(samples[0] == 0.25F);
    assert(samples[1] == -0.5F);
    assert(samples[2] == 0.75F);
    assert(samples[3] == -1.0F);
    return 0;
}
