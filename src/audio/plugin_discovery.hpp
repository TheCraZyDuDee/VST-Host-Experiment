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
            vst3_plugin_source_identity(entry.first), entry);
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
