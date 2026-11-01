#include "audio/windows_bridge_manager.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static void make_executable(const fs::path& file, const fs::path& wrapper) {
    std::ofstream script(file);
    script << "#!/bin/sh\n"
           << "case \"$1\" in\n"
           << "  status) printf \"VST3 location: '%s'\\n\" \""
           << wrapper.string() << "\" ;;\n"
           << "  list) if [ -n \"$BRIDGE_LIST\" ]; then printf '%s\\n' \"$BRIDGE_LIST\"; fi; exit 0 ;;\n"
           << "  add) printf '%s\\n' \"$2\" >> \"$BRIDGE_ADD_LOG\"; exit 0 ;;\n"
           << "  sync) exit 0 ;;\n"
           << "  *) exit 1 ;;\n"
           << "esac\n";
    script.close();
    fs::permissions(file, fs::perms::owner_exec | fs::perms::group_exec |
                          fs::perms::others_exec, fs::perm_options::add);
}

int main() {
    using namespace vst_host::audio;
    const std::string yabridge_status =
        "yabridge path: <auto>\n"
        "VST2 location: '/home/test/.vst/yabridge'\n"
        "VST3 location: '/home/test/.vst3/yabridge'\n"
        "CLAP location: '/home/test/.clap/yabridge'\n";
    const std::string vstbridge_status =
        "vstbridge path: <auto>\n"
        "VST3 location: '/home/test/.vst3/vstbridge'\n";
    assert(detail::parse_vst3_location(yabridge_status) ==
           "/home/test/.vst3/yabridge");
    assert(detail::parse_vst3_location(vstbridge_status) ==
           "/home/test/.vst3/vstbridge");
    assert(detail::parse_vst3_location("no wrapper directory").empty());

    assert(detail::preferred_bridge(true, true) == WindowsBridgeBackend::Vstbridge);
    assert(detail::preferred_bridge(true, false) == WindowsBridgeBackend::Vstbridge);
    assert(detail::preferred_bridge(false, true) == WindowsBridgeBackend::Yabridge);
    assert(!detail::preferred_bridge(false, false));

    const fs::path root = fs::temp_directory_path() /
        ("vst-host-bridge-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    const fs::path binary_dir = root / "bin";
    const fs::path native_vst = root / ".vst3/vstbridge";
    const fs::path old_vst = root / ".vst3/yabridge";
    const fs::path windows_dir = root / "Windows Plugins With Spaces";
    fs::create_directories(binary_dir);
    fs::create_directories(native_vst);
    fs::create_directories(old_vst);
    fs::create_directories(windows_dir);
    const auto new_bundle = native_vst / "My Effect.vst3";
    const auto old_bundle = old_vst / "My Effect.vst3";
    fs::create_directories(new_bundle);
    fs::create_directories(old_bundle);

    const fs::path new_ctl = binary_dir / "vstbridgectl";
    const fs::path old_ctl = binary_dir / "yabridgectl";
    make_executable(new_ctl, native_vst);
    make_executable(old_ctl, old_vst);

    setenv("HOME", root.c_str(), 1);
    setenv("PATH", binary_dir.c_str(), 1);
    const auto add_log = root / "registered-adds.log";
    setenv("BRIDGE_ADD_LOG", add_log.c_str(), 1);
    unsetenv("BRIDGE_LIST");
    unsetenv("VSTBRIDGECTL");
    unsetenv("YABRIDGECTL");
    auto preferred = detail::locate_bridge();
    assert(preferred && preferred->backend == WindowsBridgeBackend::Vstbridge);

    PipeWireOptions options;
    options.windows_plugin_directories = {windows_dir.string()};
    std::string error;
    WindowsBridgeSelection selection;
    assert(prepare_windows_vst3_plugins(options, error, &selection));
    assert(selection.backend == WindowsBridgeBackend::Vstbridge);
    assert(selection.wrapper_directory == native_vst);
    assert(options.plugin_directories.size() == 1);
    assert(options.plugin_directories.front() == native_vst.string());
    assert(detail::migrate_vst3_wrapper_path(old_bundle.string(), selection) ==
           new_bundle.string());
    assert(detail::migrate_vst3_wrapper_path("/other/native.vst3", selection) ==
           "/other/native.vst3");
    assert(detail::migrate_vst3_wrapper_path(
        (old_vst / "Not Yet Synced.vst3").string(), selection) ==
        (old_vst / "Not Yet Synced.vst3").string());

    // Keep the previous bridge working for people who have not installed
    // vstbridge. An explicit YABRIDGECTL also overrides auto-preference.
    setenv("YABRIDGECTL", old_ctl.c_str(), 1);
    auto forced = detail::locate_bridge();
    assert(forced && forced->backend == WindowsBridgeBackend::Yabridge);
    PipeWireOptions fallback;
    fallback.windows_plugin_directories = {windows_dir.string()};
    assert(prepare_windows_vst3_plugins(fallback, error, &selection));
    assert(selection.backend == WindowsBridgeBackend::Yabridge);
    assert(selection.wrapper_directory == old_vst);
    assert(detail::migrate_vst3_wrapper_path(new_bundle.string(), selection) ==
           old_bundle.string());

    // The Windows bridge's 'list' command prints absolute registered roots
    // one per line; paths can contain spaces. A parent covers its children.
    const auto nested = windows_dir / "VST3";
    fs::create_directories(nested);
    const auto list = windows_dir.string() + "\n" + nested.string() + "\n";
    auto registered = detail::parse_registered_plugin_roots(list);
    assert(registered.size() == 2);
    assert(detail::directory_covers(windows_dir, nested));
    assert(detail::directory_covers(windows_dir, windows_dir));
    assert(!detail::directory_covers(nested, windows_dir));

    // When the parent is already registered globally, scanning the nested
    // VST3 folder must not add a second root and create mirrored wrappers.
    setenv("BRIDGE_LIST", windows_dir.c_str(), 1);
    std::ofstream(add_log, std::ios::trunc).close();
    PipeWireOptions covered;
    covered.windows_plugin_directories = {nested.string()};
    std::string warning;
    assert(prepare_windows_vst3_plugins(covered, error, &selection, &warning));
    {
        std::ifstream recorded(add_log);
        assert(recorded.peek() == std::ifstream::traits_type::eof());
    }

    // If both directories are newly requested, parent-first registration
    // avoids the same overlap even if they were configured in reverse order.
    unsetenv("BRIDGE_LIST");
    std::ofstream(add_log, std::ios::trunc).close();
    PipeWireOptions combined;
    combined.windows_plugin_directories = {nested.string(), windows_dir.string()};
    assert(prepare_windows_vst3_plugins(combined, error, &selection));
    {
        std::ifstream recorded(add_log);
        std::string line;
        assert(std::getline(recorded, line) && line == windows_dir.string());
        assert(!std::getline(recorded, line));
    }

    // Existing overlapping global registrations are never deleted by us;
    // the GUI should instead explain how to inspect and prune them.
    setenv("BRIDGE_LIST", list.c_str(), 1);
    warning.clear();
    PipeWireOptions already_duplicated;
    already_duplicated.windows_plugin_directories = {nested.string()};
    assert(prepare_windows_vst3_plugins(
        already_duplicated, error, &selection, &warning));
    assert(warning.find("overlapping globally registered") != std::string::npos);
    unsetenv("BRIDGE_LIST");

    setenv("YABRIDGECTL", (root / "missing-tool").c_str(), 1);
    assert(!detail::locate_bridge());
    unsetenv("YABRIDGECTL");

    fs::remove_all(root);
    return 0;
}
