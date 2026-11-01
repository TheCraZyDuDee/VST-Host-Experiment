#include "audio/pipewire_engine.hpp"
#include "audio/config.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

void print_usage(const char* executable) {
    std::cout
        << "Usage: " << executable << " [options]\n\n"
        << "Start a lightweight PipeWire virtual microphone with optional VST2/VST3 effects.\n\n"
        << "Options:\n"
        << "  --config FILE       Load a saved host configuration (headless mode)\n"
        << "  --save-config FILE  Save effective options before starting\n"
        << "  --input NODE        Stable PipeWire microphone node name\n"
        << "  --name NODE_NAME    Virtual microphone node name\n"
        << "  --plugin PATH       Add an enabled VST2 .so / VST3 .vst3 effect\n"
        << "  --plugin-bypassed PATH  Add a VST2/VST3 effect initially bypassed\n"
        << "  --plugin-dir PATH   Scan a native Linux plugin directory; repeat as needed\n"
        << "  --windows-plugin-dir PATH  Register Windows VST2/VST3 directory (Wine/vstbridge)\n"
        << "  --gain VALUE        Linear gain from 0.0 to 4.0\n"
        << "  --latency-frames N  128, 256, 512, or 1024\n"
        << "  --bypass            Bypass the gain processor\n"
        << "  --help              Show this help\n";
}

bool parse_gain(std::string_view text, float& gain) {
    std::string owned(text);
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(owned.c_str(), &end);
    if (errno != 0 || end == owned.c_str() || *end != '\0' ||
        !std::isfinite(parsed) || parsed < 0.0F || parsed > 4.0F) return false;
    gain = parsed;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::string save_config_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--config" || arg == "--save-config") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << ".\n";
                return 2;
            }
            if (arg == "--config") config_path = argv[i + 1];
            else save_config_path = argv[i + 1];
            ++i;
        }
    }

    vst_host::audio::PipeWireOptions options;
    if (!config_path.empty()) {
        std::string error;
        if (!vst_host::audio::load_config(config_path, options, error)) {
            std::cerr << error << '\n';
            return 2;
        }
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--config" || arg == "--save-config") { ++i; continue; }
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--bypass") {
            options.bypass = true;
            continue;
        }
        if (arg == "--input" || arg == "--name" || arg == "--plugin" ||
            arg == "--plugin-bypassed" || arg == "--plugin-dir" ||
            arg == "--windows-plugin-dir" ||
            arg == "--gain" || arg == "--latency-frames") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << ".\n";
                print_usage(argv[0]);
                return 2;
            }
            const std::string_view value(argv[++i]);
            if (arg == "--input") {
                options.input_target = value;
            } else if (arg == "--name") {
                if (value.empty()) {
                    std::cerr << "Virtual microphone name cannot be empty.\n";
                    return 2;
                }
                options.output_name = value;
                options.output_description = value;
            } else if (arg == "--windows-plugin-dir") {
                if (value.empty()) {
                    std::cerr << "Windows plugin directory cannot be empty.\n";
                    return 2;
                }
                options.windows_plugin_directories.emplace_back(value);
                // CLI directory flags explicitly opt in to loading discovered
                // effects. Saved .vsthost directory entries remain scan-only.
                options.auto_load_windows_plugin_directories.emplace_back(value);
            } else if (arg == "--plugin-dir") {
                if (value.empty()) {
                    std::cerr << "Plugin directory cannot be empty.\n";
                    return 2;
                }
                options.plugin_directories.emplace_back(value);
                options.auto_load_plugin_directories.emplace_back(value);
            } else if (arg == "--plugin" || arg == "--plugin-bypassed") {
                if (value.empty()) {
                    std::cerr << "Plugin path cannot be empty.\n";
                    return 2;
                }
                options.plugin_paths.emplace_back(value);
                options.plugin_bypassed.push_back(arg == "--plugin-bypassed");
            } else if (arg == "--gain") {
                if (!parse_gain(value, options.gain)) {
                    std::cerr << "Gain must be a number between 0.0 and 4.0.\n";
                    return 2;
                }
            } else {
                if (value == "128") options.latency_frames = 128;
                else if (value == "256") options.latency_frames = 256;
                else if (value == "512") options.latency_frames = 512;
                else if (value == "1024") options.latency_frames = 1024;
                else {
                    std::cerr << "Latency frames must be 128, 256, 512, or 1024.\n";
                    return 2;
                }
            }
            continue;
        }
        std::cerr << "Unknown option: " << arg << '\n';
        print_usage(argv[0]);
        return 2;
    }

    if (!save_config_path.empty()) {
        std::string error;
        if (!vst_host::audio::save_config(save_config_path, options, error)) {
            std::cerr << error << '\n';
            return 2;
        }
    }

    vst_host::audio::PipeWireEngine engine(std::move(options));
    return engine.run() == 0 ? 0 : 1;
}
