#include "audio/pipewire_engine.hpp"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace {

void print_usage(const char* executable) {
    std::cout
        << "Usage: " << executable << " [options]\n\n"
        << "Start a lightweight PipeWire microphone pass-through with optional VST3 effect.\n\n"
        << "Options:\n"
        << "  --input NODE       PipeWire microphone node id/name (default: default source)\n"
        << "  --name NODE_NAME   Virtual microphone node name\n"
        << "  --plugin PATH      Load a native Linux VST3 effect bundle (optional)\n"
        << "  --gain VALUE       Linear gain from 0.0 to 4.0 (default: 1.0)\n"
        << "  --latency-frames N 128, 256, 512, or 1024 (default: 128)\n"
        << "  --bypass           Bypass the gain processor\n"
        << "  --help             Show this help\n";
}

bool parse_gain(std::string_view text, float& gain) {
    std::string owned(text);
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(owned.c_str(), &end);
    if (errno != 0 || end == owned.c_str() || *end != '\0' ||
        !std::isfinite(parsed) || parsed < 0.0F || parsed > 4.0F) {
        return false;
    }
    gain = parsed;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    vst_host::audio::PipeWireOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--bypass") {
            options.bypass = true;
            continue;
        }
        if (arg == "--input" || arg == "--name" || arg == "--plugin" ||
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
            } else if (arg == "--plugin") {
                if (value.empty()) {
                    std::cerr << "Plugin path cannot be empty.\n";
                    return 2;
                }
                options.plugin_path = value;
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

        std::cerr << "Unknown option: " << arg << "\n";
        print_usage(argv[0]);
        return 2;
    }

    vst_host::audio::PipeWireEngine engine(std::move(options));
    return engine.run() == 0 ? 0 : 1;
}
