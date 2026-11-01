#pragma once

#include "pipewire_engine.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace vst_host::audio {

enum class WindowsBridgeBackend { Vstbridge, Yabridge };

inline constexpr std::string_view bridge_name(WindowsBridgeBackend backend) noexcept {
    return backend == WindowsBridgeBackend::Vstbridge ? "vstbridge" : "yabridge";
}

struct WindowsBridgeSelection {
    WindowsBridgeBackend backend{WindowsBridgeBackend::Vstbridge};
    std::string executable;
    std::filesystem::path wrapper_directory;
};

namespace detail {

// Tests and callers can select explicitly; the default is vstbridge when both
// tools are installed, unless YABRIDGECTL was explicitly requested.
inline std::optional<WindowsBridgeBackend> preferred_bridge(
    bool vstbridge_available, bool yabridge_available) noexcept {
    if (vstbridge_available) return WindowsBridgeBackend::Vstbridge;
    if (yabridge_available) return WindowsBridgeBackend::Yabridge;
    return std::nullopt;
}

inline bool run_bridge_command(const std::vector<std::string>& arguments,
                               std::string& output, std::string& error) {
    if (arguments.empty()) {
        error = "No Windows bridge command was supplied.";
        return false;
    }
    int pipe_fds[2]{};
    if (pipe(pipe_fds) != 0) {
        error = "Could not create a bridge output pipe: " +
                std::string(std::strerror(errno));
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    pid_t child{};
    const int spawn_result = posix_spawnp(
        &child, arguments.front().c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    if (spawn_result != 0) {
        close(pipe_fds[0]);
        error = "Could not start '" + arguments.front() + "': " +
                std::string(std::strerror(spawn_result));
        return false;
    }

    char buffer[4096];
    for (;;) {
        const auto bytes = read(pipe_fds[0], buffer, sizeof(buffer));
        if (bytes > 0) {
            if (output.size() < 1024 * 1024)
                output.append(buffer, std::min(static_cast<std::size_t>(bytes),
                                                1024 * 1024 - output.size()));
            continue;
        }
        if (bytes < 0 && errno == EINTR) continue;
        break;
    }
    close(pipe_fds[0]);

    int status{};
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        error = "Could not wait for the bridge tool: " + std::string(std::strerror(errno));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = arguments.front() + " command failed";
        if (WIFEXITED(status))
            error += " (exit code " + std::to_string(WEXITSTATUS(status)) + ")";
        if (!output.empty()) error += ":\n" + output;
        return false;
    }
    return true;
}

inline std::string parse_vst3_location(const std::string& status_output) {
    constexpr std::string_view prefix = "VST3 location: '";
    std::size_t position = 0;
    while ((position = status_output.find(prefix, position)) != std::string::npos) {
        const auto begin = position + prefix.size();
        const auto end = status_output.find('\'', begin);
        if (end != std::string::npos) return status_output.substr(begin, end - begin);
        position = begin;
    }
    return {};
}

// A bridge tool maintains a GLOBAL set of plugin search roots, shared by all
// audio hosts. If it has both a parent root and its VST3 child, syncing can
// generate two differently nested wrappers for the very same Windows module.
inline bool directory_covers(const std::filesystem::path& parent,
                             const std::filesystem::path& child) {
    auto ancestor = parent.begin();
    auto descendant = child.begin();
    for (; ancestor != parent.end(); ++ancestor, ++descendant) {
        if (descendant == child.end() || *ancestor != *descendant)
            return false;
    }
    return true;
}

inline std::vector<std::filesystem::path> parse_registered_plugin_roots(
    const std::string& listing) {
    std::vector<std::filesystem::path> roots;
    std::istringstream input(listing);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::filesystem::path path(line);
        if (!path.is_absolute()) continue;
        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(path, ec);
        roots.push_back(ec ? path.lexically_normal() : canonical);
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

inline bool is_executable(const std::filesystem::path& candidate) {
    std::error_code ec;
    return std::filesystem::is_regular_file(candidate, ec) && !ec &&
           access(candidate.c_str(), X_OK) == 0;
}

inline std::string find_in_path(std::string_view name) {
    if (name.find('/') != std::string_view::npos)
        return is_executable(std::filesystem::path(name)) ? std::string(name) : std::string{};
    const char* path = std::getenv("PATH");
    if (!path) return {};
    std::string_view remaining(path);
    for (;;) {
        const auto colon = remaining.find(':');
        const auto segment = remaining.substr(0, colon);
        const auto base = segment.empty() ? std::filesystem::path(".")
                                          : std::filesystem::path(segment);
        const auto candidate = base / std::string(name);
        if (is_executable(candidate)) return candidate.string();
        if (colon == std::string_view::npos) break;
        remaining.remove_prefix(colon + 1);
    }
    return {};
}

inline std::string locate_control(WindowsBridgeBackend backend) {
    const auto tool = bridge_name(backend);
    const char* override_var = backend == WindowsBridgeBackend::Vstbridge
        ? "VSTBRIDGECTL" : "YABRIDGECTL";
    if (const char* override_path = std::getenv(override_var);
        override_path && *override_path)
        return find_in_path(override_path);

    if (const char* home = std::getenv("HOME"); home && *home) {
        const auto local = std::filesystem::path(home) / ".local/share" /
                           std::string(tool) / (std::string(tool) + "ctl");
        if (is_executable(local)) return local.string();
    }
    return find_in_path(std::string(tool) + "ctl");
}

inline std::optional<WindowsBridgeSelection> locate_bridge() {
    // Explicit per-backend overrides take priority over automatic preference.
    if (const char* p = std::getenv("VSTBRIDGECTL"); p && *p) {
        const auto control = locate_control(WindowsBridgeBackend::Vstbridge);
        if (!control.empty()) return WindowsBridgeSelection{WindowsBridgeBackend::Vstbridge, control, {}};
        return std::nullopt;
    }
    if (const char* p = std::getenv("YABRIDGECTL"); p && *p) {
        const auto control = locate_control(WindowsBridgeBackend::Yabridge);
        if (!control.empty()) return WindowsBridgeSelection{WindowsBridgeBackend::Yabridge, control, {}};
        return std::nullopt;
    }
    const auto vstbridge = locate_control(WindowsBridgeBackend::Vstbridge);
    if (!vstbridge.empty())
        return WindowsBridgeSelection{WindowsBridgeBackend::Vstbridge, vstbridge, {}};
    const auto yabridge = locate_control(WindowsBridgeBackend::Yabridge);
    if (!yabridge.empty())
        return WindowsBridgeSelection{WindowsBridgeBackend::Yabridge, yabridge, {}};
    return std::nullopt;
}

// Return the equivalent wrapper from the selected bridge if it exists. An
// unsynced/missing target is never substituted for a known loadable path.
inline std::string migrate_vst3_wrapper_path(const std::string& path,
                                             const WindowsBridgeSelection& selected) {
    constexpr std::string_view vstbridge_marker = "/.vst3/vstbridge/";
    constexpr std::string_view yabridge_marker = "/.vst3/yabridge/";
    const auto marker = selected.backend == WindowsBridgeBackend::Vstbridge
        ? yabridge_marker : vstbridge_marker;
    const auto position = path.find(marker);
    if (position == std::string::npos || selected.wrapper_directory.empty()) return path;

    const auto relative = path.substr(position + marker.size());
    const auto candidate = selected.wrapper_directory / relative;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && !ec) return candidate.string();
    return path;
}

} // namespace detail

// Register and sync Windows VST3 plugins before loading. vstbridge is preferred
// when installed. yabridge is a fallback so existing setups keep working.
// Both tools have the same add/sync/status CLI and output a native VST3 wrapper.
inline bool prepare_windows_vst3_plugins(PipeWireOptions& options, std::string& error,
                                         WindowsBridgeSelection* selected = nullptr,
                                         std::string* warning = nullptr) {
    if (options.windows_plugin_directories.empty()) return true;

    auto bridge = detail::locate_bridge();
    if (!bridge) {
        error = "No Windows VST3 bridge found. Install vstbridge (preferred) with "
                "vstbridgectl, or yabridge with yabridgectl. "
                "Set VSTBRIDGECTL or YABRIDGECTL to override its path.";
        return false;
    }
    const auto bridge_label = std::string(bridge_name(bridge->backend));

    // Read existing bridge registrations before adding new roots. We must
    // not remove registrations here: they may belong to other DAWs.
    std::string registered_output;
    std::string list_error;
    std::vector<std::filesystem::path> registered;
    if (detail::run_bridge_command(
            {bridge->executable, "list"}, registered_output, list_error))
        registered = detail::parse_registered_plugin_roots(registered_output);

    std::vector<std::filesystem::path> requested;
    requested.reserve(options.windows_plugin_directories.size());
    for (const auto& directory : options.windows_plugin_directories) {
        std::error_code ec;
        const std::filesystem::path path(directory);
        if (!std::filesystem::is_directory(path, ec) || ec) {
            error = "Windows VST3 directory is unavailable: " + directory;
            return false;
        }
        const auto canonical_path = std::filesystem::canonical(path, ec);
        if (ec) {
            error = "Could not resolve Windows VST3 directory: " + directory +
                    " (" + ec.message() + ")";
            return false;
        }
        requested.push_back(canonical_path);
    }
    // Parent roots first, so a child added in the same scan never creates a
    // redundant registration. Already registered parents are also respected.
    std::sort(requested.begin(), requested.end(), [](const auto& a, const auto& b) {
        return a.string().size() < b.string().size() ||
               (a.string().size() == b.string().size() && a < b);
    });
    requested.erase(std::unique(requested.begin(), requested.end()), requested.end());

    bool overlapping_old_roots = false;
    for (std::size_t i = 0; i < registered.size(); ++i) {
        for (std::size_t j = i + 1; j < registered.size(); ++j) {
            if (detail::directory_covers(registered[i], registered[j]) ||
                detail::directory_covers(registered[j], registered[i]))
                overlapping_old_roots = true;
        }
    }
    if (overlapping_old_roots && warning) {
        *warning += bridge_label + " has overlapping globally registered plugin roots. "
                    "This may have generated duplicate wrappers. Inspect '"
                    + bridge->executable + " list' before removing redundant roots and "
                    "running 'sync --prune'. Existing registrations were not modified.\n";
    }

    for (const auto& directory : requested) {
        bool covered = false;
        for (const auto& registered_dir : registered) {
            if (detail::directory_covers(registered_dir, directory)) {
                covered = true;
                break;
            }
        }
        if (covered) continue;

        bool overlaps_child = false;
        for (const auto& registered_dir : registered) {
            if (detail::directory_covers(directory, registered_dir)) {
                overlaps_child = true;
                break;
            }
        }
        if (overlaps_child && warning)
            *warning += "Adding " + directory.string() + " to " + bridge_label +
                " overlaps an already registered child directory. "
                "Review '" + bridge->executable + " list' for duplicate roots.\n";

        std::string output;
        if (!detail::run_bridge_command({bridge->executable, "add", directory.string()},
                                        output, error)) {
            error = "Could not register Windows VST3 directory '" + directory.string() +
                    "' with " + bridge_label + ".\n" + error;
            return false;
        }
        registered.push_back(directory);
    }

    std::string sync_output;
    if (!detail::run_bridge_command({bridge->executable, "sync"}, sync_output, error)) {
        error = "Could not synchronize Windows VST3 plugins using " + bridge_label +
                ". Verify that Wine and " + bridge_label + " are installed and compatible.\n" + error;
        return false;
    }

    std::string status_output;
    std::string status_error;
    std::string wrapper_directory;
    if (detail::run_bridge_command({bridge->executable, "status"}, status_output, status_error))
        wrapper_directory = detail::parse_vst3_location(status_output);

    if (wrapper_directory.empty()) {
        if (const char* home = std::getenv("HOME"); home && *home)
            wrapper_directory = (std::filesystem::path(home) / ".vst3" /
                                 bridge_label).string();
    }
    std::error_code wrapper_error;
    if (wrapper_directory.empty() ||
        !std::filesystem::is_directory(wrapper_directory, wrapper_error) || wrapper_error) {
        error = bridge_label +
                " synchronized, but its generated VST3 wrapper directory could not be located.";
        if (!status_error.empty()) error += "\n" + status_error;
        return false;
    }
    bridge->wrapper_directory = wrapper_directory;
    if (std::find(options.plugin_directories.begin(), options.plugin_directories.end(),
                  wrapper_directory) == options.plugin_directories.end())
        options.plugin_directories.push_back(wrapper_directory);
    if (selected) *selected = *bridge;
    return true;
}

} // namespace vst_host::audio
