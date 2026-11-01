#include "audio/plugin_state.hpp"

#include <cstdint>
#include <string>
#include <vector>

int main() {
    using vst_host::audio::base64_encode;
    using vst_host::audio::base64_decode;

    std::vector<std::uint8_t> output;
    if (base64_encode({}) != "") return 1;
    if (base64_encode({0x66}) != "Zg==") return 2;
    if (base64_encode({0x66, 0x6f}) != "Zm8=") return 3;
    if (base64_encode({0x66, 0x6f, 0x6f}) != "Zm9v") return 4;

    std::vector<std::uint8_t> input{0, 255, 1, 2, 3, 4, 5, 0, 254};
    if (!base64_decode(base64_encode(input), output) || output != input) return 5;
    if (!base64_decode("", output) || !output.empty()) return 6;
    const std::vector<std::string> malformed{
        "A", "Zg=", "Zg===", "Zg=a", "A===", "Zg==Zg==",
        "Zg$=", "Zg=Z", "Zm9=", "Zq==", "===="
    };
    for (const auto& text : malformed) {
        if (base64_decode(text, output)) return 7;
    }
    if (base64_decode(std::string(4 * ((vst_host::audio::max_plugin_state_bytes + 2) / 3) + 4, 'A'),
                      output)) return 8;
    return 0;
}
