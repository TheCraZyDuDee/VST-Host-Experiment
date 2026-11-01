#pragma once

#include "plugin_discovery.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vst_host::audio {

// vstbridge/yabridge have a global registry shared with other audio hosts.
// Their centralized ~/.vst and ~/.vst3 trees may contain plugins registered
// by unrelated programs. Scan the wrappers, but only present plugins whose
// ORIGINAL Windows module resides in a directory the user selected here.
//
// Compare canonical path components rather than string prefixes: selecting
// /Windows/VST2 must never include /Windows/VST3 or /Windows/VST2-Other.
inline bool path_within_directory(const std::filesystem::path& path,
                                  const std::filesystem::path& root) {
    const auto parent = root.lexically_normal();
    const auto child = path.lexically_normal();
    auto i = parent.begin();
    auto j = child.begin();
    for (; i != parent.end(); ++i, ++j) {
        if (j == child.end() || *i != *j) return false;
    }
    return true;
}

inline std::vector<std::filesystem::path> bridged_windows_sources(
    const std::string& wrapper_path) {
    namespace fs = std::filesystem;
    const fs::path wrapper(wrapper_path);
    std::vector<fs::path> sources;

    if (plugin_format(wrapper_path) == PluginFormat::Vst2) {
        // In centralized mode a bridge .so has an adjacent original .dll
        // symlink. Inline installations may instead have the actual .dll
        // next to the .so, so support both cases.
        auto dll = wrapper;
        dll.replace_extension(".dll");
        std::error_code ec;
        if (fs::is_regular_file(dll, ec) && !ec) {
            const auto source = fs::canonical(dll, ec);
            if (!ec) sources.push_back(source);
        }
    } else if (plugin_format(wrapper_path) == PluginFormat::Vst3) {
        for (std::string_view arch :
             {"x86_64-win", "x86-win", "aarch64-win", "arm64-win"}) {
            std::error_code ec;
            fs::directory_iterator it(
                wrapper / "Contents" / arch,
                fs::directory_options::skip_permission_denied, ec);
            if (ec) continue;
            const fs::directory_iterator end;
            while (it != end) {
                const auto& entry = *it;
                std::error_code link_ec;
                if (entry.path().extension() == ".vst3" &&
                    entry.is_symlink(link_ec) && !link_ec) {
                    const auto source = fs::canonical(entry.path(), link_ec);
                    if (!link_ec) sources.push_back(source);
                }
                it.increment(ec);
                if (ec) break;
            }
        }
        // Inline Wine wrappers can be created inside the original Windows
        // .vst3 bundle rather than via a separate x86_64-win symlink.
        // Only accept this fallback if a Linux binary is actually present.
        if (sources.empty()) {
            const auto contents = wrapper / "Contents";
            for (std::string_view arch :
                 {"x86_64-linux", "i386-linux", "aarch64-linux"}) {
                std::error_code ec;
                const auto linux_dir = contents / arch;
                fs::directory_iterator it(
                    linux_dir, fs::directory_options::skip_permission_denied, ec);
                if (ec) continue;
                const fs::directory_iterator end;
                while (it != end) {
                    const auto& entry = *it;
                    std::error_code file_ec;
                    if (entry.path().extension() == ".so" &&
                        entry.is_regular_file(file_ec) && !file_ec) {
                        const auto source = fs::canonical(wrapper, file_ec);
                        if (!file_ec) sources.push_back(source);
                        break;
                    }
                    it.increment(ec);
                    if (ec) break;
                }
                if (!sources.empty()) break;
            }
        }
    }
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    return sources;
}

inline bool windows_wrapper_in_selected_directories(
    const std::string& wrapper_path,
    const std::vector<std::string>& selected_windows_roots) {
    if (selected_windows_roots.empty()) return false;
    const auto sources = bridged_windows_sources(wrapper_path);
    // Unknown/broken Windows wrapper origins are NOT treated as a match.
    if (sources.empty()) return false;

    namespace fs = std::filesystem;
    for (const auto& name : selected_windows_roots) {
        std::error_code ec;
        const auto root = fs::canonical(fs::path(name), ec);
        if (ec) continue;
        for (const auto& source : sources)
            if (path_within_directory(source, root)) return true;
    }
    return false;
}

// Preserve order and format while filtering only bridge-derived discoveries.
// Explicit processing-chain entries are never removed or changed by this.
inline std::vector<std::string> filter_bridge_plugins_to_selected_roots(
    const std::vector<std::string>& wrappers,
    const std::vector<std::string>& selected_windows_roots,
    std::size_t* excluded = nullptr) {
    std::vector<std::string> selected;
    selected.reserve(wrappers.size());
    for (const auto& path : wrappers) {
        if (windows_wrapper_in_selected_directories(path, selected_windows_roots))
            selected.push_back(path);
    }
    if (excluded) *excluded = wrappers.size() - selected.size();
    return selected;
}

} // namespace vst_host::audio
