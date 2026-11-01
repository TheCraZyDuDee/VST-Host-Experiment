#pragma once

#include "pipewire_engine.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace vst_host::audio {

// Persisted directory entries are search roots for a GUI plugin library, NOT
// instructions to load every installed effect. Only an explicit headless
// --plugin-dir / --windows-plugin-dir opts into processing additional effects.
inline void add_headless_autoload_directory(PipeWireOptions& options,
                                            std::string_view directory,
                                            bool windows) {
    std::string owned(directory);
    if (windows) {
        options.windows_plugin_directories.push_back(owned);
        options.auto_load_windows_plugin_directories.push_back(std::move(owned));
    } else {
        options.plugin_directories.push_back(owned);
        options.auto_load_plugin_directories.push_back(std::move(owned));
    }
}

} // namespace vst_host::audio
