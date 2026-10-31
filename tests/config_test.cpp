#include "audio/config.hpp"

#include <cassert>
#include <filesystem>
#include <string>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "vst-host-config-test.vsthost";
    vst_host::audio::PipeWireOptions source;
    source.input_target = "USB Mic (Studio)";
    source.output_name = "My Virtual Mic";
    source.gain = 1.75F;
    source.latency_frames = 512;
    source.bypass = true;
    source.plugin_paths = {"/plugins/First Effect.vst3", "/plugins/Second.vst3"};

    std::string error;
    assert(vst_host::audio::save_config(path, source, error));
    vst_host::audio::PipeWireOptions restored;
    assert(vst_host::audio::load_config(path, restored, error));
    assert(restored.input_target == source.input_target);
    assert(restored.output_name == source.output_name);
    assert(restored.gain == source.gain);
    assert(restored.latency_frames == source.latency_frames);
    assert(restored.bypass == source.bypass);
    assert(restored.plugin_paths == source.plugin_paths);

    std::filesystem::remove(path);
    return 0;
}
