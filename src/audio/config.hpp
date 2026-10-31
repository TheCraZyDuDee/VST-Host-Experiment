#pragma once

#include "pipewire_engine.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace vst_host::audio {

// Portable, line-oriented configuration shared by the CLI and GUI.
// String values use C++ quoted-string escaping; repeated plugin entries preserve order.
inline bool load_config(const std::filesystem::path& path, PipeWireOptions& options,
                        std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "Could not open configuration file: " + path.string();
        return false;
    }

    PipeWireOptions parsed;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        const auto equals = line.find('=', first);
        if (equals == std::string::npos) {
            error = "Invalid configuration at line " + std::to_string(line_number);
            return false;
        }
        const auto key = line.substr(first, equals - first);
        std::istringstream value_stream(line.substr(equals + 1));
        if (key == "gain") {
            float value{};
            if (!(value_stream >> value) || !std::isfinite(value) || value < 0.0F || value > 4.0F) {
                error = "Invalid gain at line " + std::to_string(line_number);
                return false;
            }
            parsed.gain = value;
        } else if (key == "latency_frames") {
            unsigned value{};
            if (!(value_stream >> value) ||
                (value != 128 && value != 256 && value != 512 && value != 1024)) {
                error = "Invalid latency_frames at line " + std::to_string(line_number);
                return false;
            }
            parsed.latency_frames = value;
        } else if (key == "bypass") {
            std::string value;
            if (!(value_stream >> value) || (value != "true" && value != "false")) {
                error = "Invalid bypass at line " + std::to_string(line_number);
                return false;
            }
            parsed.bypass = value == "true";
        } else if (key == "input" || key == "output_name" || key == "plugin") {
            std::string value;
            if (!(value_stream >> std::quoted(value)) || value.empty()) {
                error = "Expected a non-empty quoted string at line " + std::to_string(line_number);
                return false;
            }
            value_stream >> std::ws;
            if (!value_stream.eof()) {
                error = "Unexpected trailing data at line " + std::to_string(line_number);
                return false;
            }
            if (key == "input") parsed.input_target = value;
            else if (key == "output_name") {
                parsed.output_name = value;
                parsed.output_description = value;
            } else parsed.plugin_paths.push_back(value);
        } else {
            error = "Unknown configuration key '" + key + "' at line " +
                    std::to_string(line_number);
            return false;
        }
    }
    if (!input.eof()) {
        error = "Error while reading configuration file: " + path.string();
        return false;
    }
    options = std::move(parsed);
    return true;
}

inline bool save_config(const std::filesystem::path& path, const PipeWireOptions& options,
                        std::string& error) {
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            error = "Could not write configuration file: " + temporary;
            return false;
        }
        output << "# VST Host configuration v1\n"
               << "input = " << std::quoted(options.input_target) << '\n'
               << "output_name = " << std::quoted(options.output_name) << '\n'
               << std::setprecision(9) << "gain = " << options.gain << '\n'
               << "latency_frames = " << options.latency_frames << '\n'
               << "bypass = " << (options.bypass ? "true" : "false") << '\n';
        for (const auto& plugin : options.plugin_paths)
            output << "plugin = " << std::quoted(plugin) << '\n';
        if (!output) {
            error = "Failed while writing configuration file: " + temporary;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary);
        error = "Could not finalize configuration file: " + ec.message();
        return false;
    }
    return true;
}

} // namespace vst_host::audio
