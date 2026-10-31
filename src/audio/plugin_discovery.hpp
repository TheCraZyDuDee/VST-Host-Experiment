#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
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
        }\n        std::filesystem::recursive_directory_iterator it(
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

} // namespace vst_host::audio
