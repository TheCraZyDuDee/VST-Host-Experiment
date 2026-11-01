#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <system_error>
#include <vector>

namespace vst_host::audio {

enum class PluginFormat { Vst2, Vst3, Unknown };

// Linux native VST2 plugins and bridged Windows VST2 plugins use .so files;
// native VST3 plugins and Wine VST3 wrappers use .vst3 bundles.
[[nodiscard]] inline PluginFormat plugin_format(const std::string& path) {
    const auto extension = std::filesystem::path(path).extension().string();
    if (extension == ".so") return PluginFormat::Vst2;
    if (extension == ".vst3") return PluginFormat::Vst3;
    return PluginFormat::Unknown;
}

inline std::vector<std::string> discover_vst2_plugins(
    const std::vector<std::string>& directories, std::string* warning = nullptr) {
    namespace fs = std::filesystem;
    std::vector<std::string> found;
    for (const auto& directory : directories) {
        std::error_code ec;
        const fs::path root(directory);
        if (root.extension() == ".so" && fs::is_regular_file(root, ec) && !ec) {
            found.push_back(root.lexically_normal().string());
            continue;
        }
        if (!fs::is_directory(root, ec) || ec) {
            if (warning) *warning += "Unavailable VST2 search directory: " + directory + "\n";
            continue;
        }
        fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, ec), end;
        while (it != end) {
            std::error_code entry_ec;
            const auto entry = *it;
            // Don't report shared libraries *inside* a VST3 bundle as VST2.
            if (entry.is_directory(entry_ec) && entry.path().extension() == ".vst3") {
                it.disable_recursion_pending();
            } else if (entry.path().extension() == ".so" &&
                       entry.is_regular_file(entry_ec) && !entry_ec) {
                found.push_back(entry.path().lexically_normal().string());
            }
            it.increment(ec);
            if (ec) {
                if (warning) *warning += "VST2 scan failed in " + directory + ": " +
                                        ec.message() + "\n";
                ec.clear();
            }
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

// In centralized Wine VST2 mode, the wrapper's adjacent .dll symlink points
// to the original Windows plugin. De-duplicate aliases by that target, not
// the name of the Linux .so chainloader (which is identical for every plugin).
inline std::string vst2_plugin_source_identity(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path so(path);
    auto dll = so;
    dll.replace_extension(".dll");
    std::error_code ec;
    if (fs::is_symlink(dll, ec) && !ec) {
        const auto target = fs::canonical(dll, ec);
        if (!ec) return "windows-vst2:" + target.string();
    }
    const auto resolved = fs::weakly_canonical(so, ec);
    return "vst2-bundle:" + (ec ? so.lexically_normal().string() : resolved.string());
}

// VST3 Linux bundles are directories ending in .vst3. Do not recurse into a
// discovered bundle: its internal Contents tree is not a plugin search root.
inline std::vector<std::string> discover_vst3_plugins(
    const std::vector<std::string>& directories, std::string* warning = nullptr) {
    std::vector<std::string> discovered;
    for (const auto& directory : directories) {
        std::error_code ec;
        const std::filesystem::path root(directory);
        if (!std::filesystem::is_directory(root, ec) || ec) {
            if (warning) *warning += "Plugin directory is unavailable: " + directory + "\n";
            continue;
        }
        if (root.extension() == ".vst3") {
            discovered.push_back(root.lexically_normal().string());
            continue;
        }
        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, ec), end;
        if (ec && warning)
            *warning += "Could not scan plugin directory: " + directory + "\n";
        while (it != end) {
            const auto entry = *it;
            std::error_code entry_error;
            if (entry.is_directory(entry_error) &&
                entry.path().extension() == ".vst3") {
                discovered.push_back(entry.path().lexically_normal().string());
                it.disable_recursion_pending();
            }
            it.increment(ec);
            if (ec) {
                if (warning)
                    *warning += "Error while scanning " + directory + ": " + ec.message() + "\n";
                ec.clear();
            }
        }
    }
    std::sort(discovered.begin(), discovered.end());
    discovered.erase(std::unique(discovered.begin(), discovered.end()), discovered.end());
    return discovered;
}



// vstbridge/yabridge can create several distinct wrapper directory trees
// for the SAME Windows module if overlapping search roots were registered.
// Their Linux .so files differ in path, but x86_64-win/*.vst3 symlinks point
// to the same original module. Compare those canonical targets, not filenames:
// separate plugins with the same display name must remain separate.
inline std::string vst3_plugin_source_identity(const std::string& bundle_path) {
    namespace fs = std::filesystem;
    const fs::path bundle(bundle_path);
    std::vector<std::string> targets;
    for (std::string_view architecture :
         {"x86_64-win", "x86-win", "aarch64-win", "arm64-win"}) {
        std::error_code ec;
        const auto directory = bundle / "Contents" / architecture;
        fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
        if (ec) continue;
        const fs::directory_iterator end;
        while (it != end) {
            const auto& entry = *it;
            std::error_code link_ec;
            if (entry.path().extension() == ".vst3" && entry.is_symlink(link_ec) && !link_ec) {
                const auto target = fs::canonical(entry.path(), link_ec);
                if (!link_ec) targets.push_back(target.string());
            }
            it.increment(ec);
            if (ec) break;
        }
    }

    if (!targets.empty()) {
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        std::string identity = "windows-vst3:";
        for (const auto& target : targets) {
            identity += std::to_string(target.size()) + ":" + target;
        }
        return identity;
    }

    // Preserve native plugins and broken/older wrappers by bundle identity.
    // Never merge unrelated modules just because they share a vendor/name.
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(bundle, ec);
    return "vst3-bundle:" + (ec ? bundle.lexically_normal().string() : canonical.string());
}

// Prefer the shortest relative wrapper path for a duplicated Windows module
// (e.g. 'rnnoise.vst3' rather than 'VST3/rnnoise.vst3'), consistently on
// successive scans. This only changes discovery; an explicitly selected
// processing chain may still contain two intentional instances.
// Format-qualified source IDs avoid collisions between VST2 and VST3
// bundles with similar names and preserve multiple intentional chain instances.
inline std::string plugin_source_identity(const std::string& path) {
    return plugin_format(path) == PluginFormat::Vst2
        ? vst2_plugin_source_identity(path) : plugin_source_identity(path);
}

inline bool preferable_vst3_path(const std::string& a, const std::string& b) {
    return a.size() < b.size() || (a.size() == b.size() && a < b);
}

inline std::vector<std::string> deduplicate_discovered_vst3_plugins(
    const std::vector<std::string>& paths, std::size_t* duplicate_count = nullptr) {
    std::unordered_map<std::string, std::string> by_source;
    for (const auto& path : paths) {
        auto [it, inserted] = by_source.try_emplace(vst3_plugin_source_identity(path), path);
        if (!inserted && preferable_vst3_path(path, it->second)) it->second = path;
    }
    if (duplicate_count) *duplicate_count = paths.size() - by_source.size();
    std::vector<std::string> unique;
    unique.reserve(by_source.size());
    for (const auto& [identity, path] : by_source) {
        (void)identity;
        unique.push_back(path);
    }
    std::sort(unique.begin(), unique.end());
    return unique;
}

// The GUI combines manual additions, active chain entries, Linux discoveries
// and bridge discoveries. De-duplicate that entire browser, not just the
// bridge scan. The active chain itself is never altered.
inline std::vector<std::pair<std::string, bool>> deduplicate_vst3_browser_entries(
    const std::vector<std::pair<std::string, bool>>& entries,
    std::size_t* duplicate_count = nullptr) {
    std::unordered_map<std::string, std::pair<std::string, bool>> by_source;
    for (const auto& entry : entries) {
        auto [it, inserted] = by_source.try_emplace(
            plugin_source_identity(entry.first), entry);
        if (!inserted) {
            // An entry known to be a Wine wrapper should retain its (WINE)
            // label, even if its alias was scanned as a regular Linux bundle.
            const bool wine = it->second.second || entry.second;
            if (preferable_vst3_path(entry.first, it->second.first))
                it->second.first = entry.first;
            it->second.second = wine;
        }
    }
    if (duplicate_count) *duplicate_count = entries.size() - by_source.size();
    std::vector<std::pair<std::string, bool>> unique;
    unique.reserve(by_source.size());
    for (const auto& [identity, entry] : by_source) {
        (void)identity;
        unique.push_back(entry);
    }
    std::sort(unique.begin(), unique.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    return unique;
}

} // namespace vst_host::audio
