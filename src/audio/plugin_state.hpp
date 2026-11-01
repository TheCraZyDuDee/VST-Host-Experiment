#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vst_host::audio {

// Each ordered plugin instance owns independent VST3 component (DSP) and
// controller (editor-only) snapshots. These are opaque plugin-defined bytes.
struct PluginState {
    std::vector<std::uint8_t> component;
    std::vector<std::uint8_t> controller;

    [[nodiscard]] bool empty() const noexcept {
        return component.empty() && controller.empty();
    }
};

constexpr std::size_t max_plugin_state_bytes = 16U * 1024U * 1024U;

inline std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(4 * ((bytes.size() + 2) / 3));
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<unsigned>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<unsigned>(bytes[i + 1]) : 0U;
        const auto c = i + 2 < bytes.size() ? static_cast<unsigned>(bytes[i + 2]) : 0U;
        result.push_back(alphabet[a >> 2]);
        result.push_back(alphabet[((a & 3U) << 4) | (b >> 4)]);
        result.push_back(i + 1 < bytes.size()
            ? alphabet[((b & 15U) << 2) | (c >> 6)] : '=');
        result.push_back(i + 2 < bytes.size() ? alphabet[c & 63U] : '=');
    }
    return result;
}

inline int base64_value(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Strict base64 with a hard upper bound prevents malformed or oversized
// portable config files from exhausting memory during startup.
inline bool base64_decode(std::string_view encoded,
                          std::vector<std::uint8_t>& output) {
    if (encoded.size() % 4 != 0 ||
        encoded.size() > 4 * ((max_plugin_state_bytes + 2) / 3)) return false;
    std::vector<std::uint8_t> result;
    result.reserve((encoded.size() / 4) * 3);
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        const auto a = base64_value(static_cast<unsigned char>(encoded[i]));
        const auto b = base64_value(static_cast<unsigned char>(encoded[i + 1]));
        const bool pad2 = encoded[i + 2] == '=';
        const bool pad3 = encoded[i + 3] == '=';
        const auto c = pad2 ? 0 : base64_value(static_cast<unsigned char>(encoded[i + 2]));
        const auto d = pad3 ? 0 : base64_value(static_cast<unsigned char>(encoded[i + 3]));
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (pad2 && !pad3) ||
            ((pad2 || pad3) && i + 4 != encoded.size()) ||
            (pad2 && (b & 15) != 0) || (pad3 && !pad2 && (c & 3) != 0))
            return false;
        result.push_back(static_cast<std::uint8_t>((a << 2) | (b >> 4)));
        if (!pad2)
            result.push_back(static_cast<std::uint8_t>(((b & 15) << 4) | (c >> 2)));
        if (!pad3)
            result.push_back(static_cast<std::uint8_t>(((c & 3) << 6) | d));
    }
    if (result.size() > max_plugin_state_bytes) return false;
    output = std::move(result);
    return true;
}

} // namespace vst_host::audio
