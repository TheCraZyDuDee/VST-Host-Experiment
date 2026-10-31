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
    source.plugin_directories = {"/plugins/effects", "/home/user/custom-vst3"};
    source.windows_plugin_directories = {"/home/user/.wine/drive_c/Program Files/Common Files/VST3"};

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
    assert(restored.plugin_directories == source.plugin_directories);
    assert(restored.windows_plugin_directories == source.windows_plugin_directories);

    // A blank input target means the default PipeWire microphone. GUI saves
    // this commonly, so an empty value must round-trip successfully.
    source.input_target.clear();
    if (!vst_host::audio::save_config(path, source, error)) return 1;
    restored.input_target = "not default";
    if (!vst_host::audio::load_config(path, restored, error)) return 2;
    if (!restored.input_target.empty()) return 3;

    std::filesystem::remove(path);
    return 0;
}
