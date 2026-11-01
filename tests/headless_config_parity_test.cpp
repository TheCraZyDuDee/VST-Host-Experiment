#include "audio/config.hpp"
#include "audio/headless_autoload.hpp"
#include "audio/plugin_discovery.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

int main() {
    namespace fs = std::filesystem;
    using vst_host::audio::PipeWireOptions;
    using vst_host::audio::add_headless_autoload_directory;
    using vst_host::audio::discover_vst3_plugins;
    using vst_host::audio::load_config;
    using vst_host::audio::save_config;

    const auto root = fs::temp_directory_path() /
        ("vst-host-headless-parity-" + std::to_string(getpid()));
    fs::remove_all(root);
    const auto saved_directory = root / "gui-browsed-directory";
    const auto extra_directory = root / "explicit-cli-directory";
    const auto saved_windows = root / "gui-windows-directory";
    const auto extra_windows = root / "explicit-cli-windows-directory";
    const auto unwanted = saved_directory / "Unselected.vst3";
    const auto wanted = extra_directory / "Requested.vst3";
    fs::create_directories(unwanted);
    fs::create_directories(wanted);
    fs::create_directories(saved_windows);
    fs::create_directories(extra_windows);

    // A GUI-exported configuration contains its selected chain AND library
    // search directories. Headless --config must not silently insert other
    // plugins from those directories.
    PipeWireOptions gui;
    gui.plugin_paths = {"/effects/First.vst3", "/effects/Second.so"};
    gui.plugin_bypassed = {false, true};
    gui.plugin_states.resize(2);
    gui.plugin_states[0].component = {1, 2, 3};
    gui.plugin_directories = {saved_directory.string()};
    gui.windows_plugin_directories = {saved_windows.string()};
    gui.input_target = "alsa_input.test-microphone";
    const auto file = root / "setup.vsthost";
    std::string error;
    assert(save_config(file, gui, error));

    PipeWireOptions headless;
    assert(load_config(file, headless, error));
    assert(headless.plugin_paths == gui.plugin_paths);
    assert(headless.plugin_bypassed == gui.plugin_bypassed);
    assert(headless.plugin_states.size() == 2);
    assert(headless.plugin_states[0].component == gui.plugin_states[0].component);
    assert(headless.plugin_directories == gui.plugin_directories);
    assert(headless.windows_plugin_directories == gui.windows_plugin_directories);
    assert(headless.auto_load_plugin_directories.empty());
    assert(headless.auto_load_windows_plugin_directories.empty());
    assert(discover_vst3_plugins(headless.auto_load_plugin_directories).empty());

    // Explicit --plugin-dir on a CLI command may append requested effects,
    // but never automatically loads plugins from the saved GUI scan roots.
    add_headless_autoload_directory(headless, extra_directory.string(), false);
    assert(headless.plugin_directories.size() == 2);
    assert(headless.auto_load_plugin_directories.size() == 1);
    assert(headless.auto_load_plugin_directories.front() == extra_directory.string());
    const auto additional = discover_vst3_plugins(headless.auto_load_plugin_directories);
    assert(additional.size() == 1 && additional.front() == wanted.string());
    assert(std::find(additional.begin(), additional.end(), unwanted.string()) ==
           additional.end());
    assert(headless.plugin_paths == gui.plugin_paths);

    // Likewise, --windows-plugin-dir is opt-in and independently scoped.
    // Existing Windows directories in the saved configuration remain
    // available for GUI browsing, but are not CLI auto-load targets.
    add_headless_autoload_directory(headless, extra_windows.string(), true);
    assert(headless.windows_plugin_directories.size() == 2);
    assert(headless.auto_load_windows_plugin_directories.size() == 1);
    assert(headless.auto_load_windows_plugin_directories.front() ==
           extra_windows.string());

    // Runtime-only auto-load decisions must never leak into .vsthost files.
    assert(save_config(file, headless, error));
    PipeWireOptions reloaded;
    assert(load_config(file, reloaded, error));
    assert(reloaded.auto_load_plugin_directories.empty());
    assert(reloaded.auto_load_windows_plugin_directories.empty());
    assert(reloaded.plugin_paths == gui.plugin_paths);
    assert(reloaded.plugin_directories.size() == 2);
    assert(reloaded.windows_plugin_directories.size() == 2);

    fs::remove_all(root);
    return 0;
}
