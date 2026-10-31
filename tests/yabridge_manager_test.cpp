#include "audio/yabridge_manager.hpp"

#include <cassert>
#include <string>

int main() {
    const std::string status =
        "yabridge path: <auto>\n"
        "VST2 location: '/home/test/.vst/yabridge'\n"
        "VST3 location: '/home/test/.vst3/yabridge'\n"
        "CLAP location: '/home/test/.clap/yabridge'\n";
    assert(vst_host::audio::detail::parse_vst3_location(status) ==
           "/home/test/.vst3/yabridge");
    assert(vst_host::audio::detail::parse_vst3_location("no wrapper directory") == "");
    return 0;
}
