#include "audio/vst2_processor.hpp"

#include <cassert>
#include <string>

int main() {
    vst_host::audio::Vst2Processor processor("/missing/VST2.so");
    std::string error;
    assert(!processor.load(error));
    assert(!error.empty());
    vst_host::audio::PluginState state;
    error.clear();
    assert(!processor.snapshot_plugin_state(state, error));
    assert(!error.empty());
    error.clear();
    assert(!processor.restore_plugin_state(state, error));
    assert(!error.empty());
    assert(processor.editor_parameters().empty());
    assert(!processor.set_editor_parameter(0, 0.5));
    assert(!processor.prepare({48000, 2, 128}));
    float data[]{0.25F, -0.25F};
    processor.process(data, 1);
    assert(data[0] == 0.25F && data[1] == -0.25F);
    assert(processor.latency_frames() == 0);
    return 0;
}
