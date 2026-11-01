#include "audio/config.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
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
    source.plugin_bypassed = {false, true};
    source.plugin_states.resize(2);
    source.plugin_states[0].component = {0, 255, 1, 2, 3, 4};
    source.plugin_states[0].controller = {'a', 'b', 0, 'c'};
    source.plugin_states[1].component = {8, 7, 6};
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
    assert(restored.plugin_bypassed == source.plugin_bypassed);
    assert(restored.plugin_states.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        assert(restored.plugin_states[i].component == source.plugin_states[i].component);
        assert(restored.plugin_states[i].controller == source.plugin_states[i].controller);
    }
    assert(restored.plugin_directories == source.plugin_directories);
    assert(restored.windows_plugin_directories == source.windows_plugin_directories);

    // A blank input target means the default PipeWire microphone. GUI saves
    // this commonly, so an empty value must round-trip successfully.
    source.input_target.clear();
    if (!vst_host::audio::save_config(path, source, error)) return 1;
    restored.input_target = "not default";
    if (!vst_host::audio::load_config(path, restored, error)) return 2;
    if (!restored.input_target.empty()) return 3;

    // Old configuration files omit plugin_bypassed. Every plugin should
    // start enabled even if a previous load reused the options object.
    source.plugin_bypassed.clear();
    source.plugin_states.clear();
    assert(vst_host::audio::save_config(path, source, error));
    assert(vst_host::audio::load_config(path, restored, error));
    assert(restored.plugin_bypassed.size() == 2 &&
           !restored.plugin_bypassed[0] && !restored.plugin_bypassed[1]);
    assert(restored.plugin_states.size() == 2 &&
           restored.plugin_states[0].empty() && restored.plugin_states[1].empty());

    // The bypass key cannot appear before its corresponding plugin.
    {
        std::ofstream bad(path, std::ios::trunc);
        bad << "plugin_bypassed = true\n";
    }
    assert(!vst_host::audio::load_config(path, restored, error));

    {
        std::ofstream bad(path, std::ios::trunc);
        bad << "plugin = \"effect.vst3\"\n"
               "plugin_component_state = \"not-valid-base64\"\n";
    }
    assert(!vst_host::audio::load_config(path, restored, error));
    {
        std::ofstream bad(path, std::ios::trunc);
        bad << "plugin_controller_state = \"Zg==\"\n";
    }
    assert(!vst_host::audio::load_config(path, restored, error));
    {
        std::ofstream bad(path, std::ios::trunc);
        bad << "plugin = \"effect.vst3\"\n"
               "plugin_component_state = \"Zg==\"\n"
               "plugin_component_state = \"Zg==\"\n";
    }
    assert(!vst_host::audio::load_config(path, restored, error));


    // Malformed values must fail instead of silently ignoring trailing text.
    const auto rejects = [&](const std::string& content) {
        std::ofstream bad(path, std::ios::trunc);
        bad << content;
        bad.close();
        vst_host::audio::PipeWireOptions unchanged;
        unchanged.output_name = "before failed load";
        error.clear();
        return !vst_host::audio::load_config(path, unchanged, error) &&
               !error.empty() && unchanged.output_name == "before failed load";
    };
    assert(rejects("gain = 1.5 unexpected\n"));
    assert(rejects("latency_frames = 256 trailing\n"));
    assert(rejects("bypass = true trailing\n"));
    assert(rejects("plugin = /unquoted/plugin.vst3\n"));
    assert(rejects("plugin = \"effect.vst3\"\nplugin_component_state = Zg==\n"));
    assert(rejects("plugin = \"effect.vst3\"\nplugin_bypassed = true\nplugin_bypassed = false\n"));

    // Extra whitespace is valid and older configurations remain supported.
    {
        std::ofstream valid(path, std::ios::trunc);
        valid << "gain = 1.5   \n"
                 "latency_frames = 256  \n"
                 "bypass = true   \n"
                 "plugin = \"effect.vst3\"   \n"
                 "plugin_bypassed = true   \n";
    }
    assert(vst_host::audio::load_config(path, restored, error));
    assert(restored.gain == 1.5F && restored.latency_frames == 256);
    assert(restored.bypass && restored.plugin_paths.size() == 1);
    assert(restored.plugin_bypassed.size() == 1 && restored.plugin_bypassed[0]);

    std::filesystem::remove(path);
    return 0;
}
