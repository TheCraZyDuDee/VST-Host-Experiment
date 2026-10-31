#pragma once

#include "pipewire_engine.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace vst_host::audio {
namespace detail {

inline bool run_yabridge_command(const std::vector<std::string>& arguments,
                                 std::string& output, std::string& error) {
    if (arguments.empty()) {
        error = "No yabridge command was supplied.";
        return false;
    }

    int pipe_fds[2]{};
    if (pipe(pipe_fds) != 0) {
        error = "Could not create a yabridge output pipe: " +
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
                output.append(buffer, static_cast<std::size_t>(bytes));
            continue;
        }
        if (bytes < 0 && errno == EINTR) continue;
        break;
    }
    close(pipe_fds[0]);

    int status{};
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        error = "Could not wait for yabridgectl: " + std::string(std::strerror(errno));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "yabridgectl command failed";
        if (WIFEXITED(status))
            error += " (exit code " + std::to_string(WEXITSTATUS(status)) + ")";
        if (!output.empty()) error += ":\n" + output;
        return false;
    }
    return true;
}

inline std::string locate_yabridgectl() {
    if (const char* override_path = std::getenv("YABRIDGECTL");
        override_path && *override_path &&
        std::filesystem::exists(override_path)) return override_path;

    if (const char* home = std::getenv("HOME"); home && *home) {
        const auto local = std::filesystem::path(home) /
                           ".local/share/yabridge/yabridgectl";
        if (std::filesystem::exists(local)) return local.string();
    }
    return "yabridgectl";
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

} // namespace detail

// Register configured Windows VST3 directories with yabridge and generate the
// Linux-side VST3 wrappers. This runs during startup, before PipeWire starts.
inline bool prepare_windows_vst3_plugins(PipeWireOptions& options, std::string& error) {
    if (options.windows_plugin_directories.empty()) return true;

    const auto yabridgectl = detail::locate_yabridgectl();
    for (const auto& directory : options.windows_plugin_directories) {
        std::error_code ec;
        const std::filesystem::path path(directory);
        if (!std::filesystem::is_directory(path, ec) || ec) {
            error = "Windows VST3 directory is unavailable: " + directory;
            return false;
        }
        std::string output;
        if (!detail::run_yabridge_command(
                {yabridgectl, "add", std::filesystem::canonical(path).string()},
                output, error)) {
            error = "Could not register Windows VST3 directory '" + directory +
                    "' with yabridge.\n" + error;
            return false;
        }
    }

    std::string sync_output;
    if (!detail::run_yabridge_command({yabridgectl, "sync"}, sync_output, error)) {
        error = "Could not synchronize Windows VST3 plugins. Verify that Wine and yabridge "
                "are installed and compatible.\n" + error;
        return false;
    }

    std::string status_output;
    std::string status_error;
    std::string wrapper_directory;
    if (detail::run_yabridge_command({yabridgectl, "status"}, status_output, status_error))
        wrapper_directory = detail::parse_vst3_location(status_output);
    if (wrapper_directory.empty()) {
        if (const char* home = std::getenv("HOME"); home && *home)
            wrapper_directory = (std::filesystem::path(home) / ".vst3/yabridge").string();
    }
    std::error_code wrapper_error;
    if (wrapper_directory.empty() ||
        !std::filesystem::is_directory(wrapper_directory, wrapper_error) || wrapper_error) {
        error = "yabridge synchronized, but its generated VST3 wrapper directory could not be located.";
        return false;
    }
    if (std::find(options.plugin_directories.begin(), options.plugin_directories.end(),
                  wrapper_directory) == options.plugin_directories.end())
        options.plugin_directories.push_back(wrapper_directory);
    return true;
}

} // namespace vst_host::audio
