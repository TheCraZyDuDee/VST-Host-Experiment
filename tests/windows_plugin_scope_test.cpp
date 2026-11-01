#include "audio/windows_plugin_scope.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static bool contains(const std::vector<std::string>& paths, const fs::path& path) {
    return std::find(paths.begin(), paths.end(), path.string()) != paths.end();
}

int main() {
    using namespace vst_host::audio;
    const fs::path root = fs::temp_directory_path() /
        ("vst-host-windows-plugin-scope-" + std::to_string(getpid()));
    fs::remove_all(root);
    const auto windows = root / "Windows Plugins";
    const auto vst2 = windows / "VST2";
    const auto vst3 = windows / "VST3";
    const auto unrelated = windows / "VST2-Other";
    const auto centralized = root / ".vst" / "vstbridge";
    const auto centralized_vst3 = root / ".vst3" / "vstbridge";

    fs::create_directories(vst2);
    fs::create_directories(vst3);
    fs::create_directories(unrelated);

    const auto original_compressor = vst2 / "ReaComp.dll";
    const auto original_other = unrelated / "ReaGate.dll";
    std::ofstream(original_compressor) << "VST2 plugin";
    std::ofstream(original_other) << "different VST2 plugin";

    const auto win_reacomp = centralized / "VST2" / "ReaComp.so";
    const auto win_reagate = centralized / "Other" / "ReaGate.so";
    const auto link_dll = [](const fs::path& wrapper, const fs::path& target) {
        fs::create_directories(wrapper.parent_path());
        std::ofstream(wrapper) << "Wine bridge VST2 binary";
        auto dll = wrapper;
        dll.replace_extension(".dll");
        fs::create_symlink(target, dll);
    };
    link_dll(win_reacomp, original_compressor);
    link_dll(win_reagate, original_other);

    // VST3 wrappers may refer either to a .vst3 file inside a Windows
    // bundle or to the bundle directory itself.
    const auto original_rnnoise = vst3 / "rnnoise.vst3" /
        "Contents" / "x86_64-win" / "rnnoise.vst3";
    fs::create_directories(original_rnnoise.parent_path());
    std::ofstream(original_rnnoise) << "Windows VST3 module";
    const auto original_clear = vst3 / "Supertone" / "Clear.vst3";
    fs::create_directories(original_clear);

    const auto rnnoise = centralized_vst3 / "VST3" / "rnnoise.vst3";
    const auto clear = centralized_vst3 / "Supertone" / "Clear.vst3";
    const auto make_vst3 = [](const fs::path& wrapper, const fs::path& source) {
        const auto win = wrapper / "Contents" / "x86_64-win";
        fs::create_directories(win);
        fs::create_symlink(source, win / source.filename());
    };
    make_vst3(rnnoise, original_rnnoise);
    make_vst3(clear, original_clear);

    const auto roots_vst2 = std::vector<std::string>{vst2.string()};
    const auto roots_vst3 = std::vector<std::string>{vst3.string()};
    const auto roots_all = std::vector<std::string>{windows.string()};

    if (!path_within_directory(original_compressor, vst2) ||
        path_within_directory(original_other, vst2) ||
        path_within_directory(original_rnnoise, vst2))
        return 1;
    if (!windows_wrapper_in_selected_directories(win_reacomp.string(), roots_vst2) ||
        windows_wrapper_in_selected_directories(win_reagate.string(), roots_vst2) ||
        windows_wrapper_in_selected_directories(rnnoise.string(), roots_vst2) ||
        windows_wrapper_in_selected_directories(clear.string(), roots_vst2))
        return 2;
    if (!windows_wrapper_in_selected_directories(rnnoise.string(), roots_vst3) ||
        !windows_wrapper_in_selected_directories(clear.string(), roots_vst3) ||
        windows_wrapper_in_selected_directories(win_reacomp.string(), roots_vst3))
        return 3;

    const std::vector<std::string> global{
        win_reacomp.string(), rnnoise.string(), clear.string(), win_reagate.string()};
    std::size_t excluded = 0;
    const auto only_vst2 = filter_bridge_plugins_to_selected_roots(
        global, roots_vst2, &excluded);
    if (only_vst2.size() != 1 || excluded != 3 ||
        !contains(only_vst2, win_reacomp)) return 4;
    const auto only_vst3 = filter_bridge_plugins_to_selected_roots(
        global, roots_vst3, &excluded);
    if (only_vst3.size() != 2 || excluded != 2 ||
        !contains(only_vst3, rnnoise) || !contains(only_vst3, clear)) return 5;
    const auto all = filter_bridge_plugins_to_selected_roots(
        global, roots_all, &excluded);
    if (all.size() != 4 || excluded != 0) return 6;

    // A path prefix must not count as a directory relationship.
    const auto similar = std::vector<std::string>{(windows / "VST").string()};
    if (windows_wrapper_in_selected_directories(win_reacomp.string(), similar)) return 7;
    if (windows_wrapper_in_selected_directories(win_reacomp.string(), {})) return 8;

    // Inline VST2: the .so and a real, non-symlink .dll share a directory.
    const auto inline_so = vst2 / "Inline.so";
    const auto inline_dll = vst2 / "Inline.dll";
    std::ofstream(inline_so) << "inline bridge library";
    std::ofstream(inline_dll) << "original Windows module";
    if (!windows_wrapper_in_selected_directories(inline_so.string(), roots_vst2)) return 9;

    // An unrecognized broken wrapper, even within the output directory,
    // must not leak into the library as a plugin from some other root.
    const auto broken = centralized / "Unknown.so";
    std::ofstream(broken) << "unknown";
    if (windows_wrapper_in_selected_directories(broken.string(), roots_all)) return 10;

    // An original Windows VST3 folder without an embedded Linux .so is not a
    // bridge wrapper, even if an original .vst3 file is present in the tree.
    if (windows_wrapper_in_selected_directories(
            (vst3 / "rnnoise.vst3").string(), roots_vst3))
        return 11;

    // Inline VST3 with native Linux wrapper installed inside the bundle.
    const auto linux_dir = original_clear / "Contents" / "x86_64-linux";
    fs::create_directories(linux_dir);
    std::ofstream(linux_dir / "Clear.so") << "inline bridge VST3 binary";
    if (!windows_wrapper_in_selected_directories(original_clear.string(), roots_vst3))
        return 12;

    fs::remove_all(root);
    return 0;
}
