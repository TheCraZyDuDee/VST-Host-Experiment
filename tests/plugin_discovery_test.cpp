#include "audio/plugin_discovery.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "vst-host-plugin-scan-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "vendor-a" / "Alpha.vst3" / "Contents");
    std::filesystem::create_directories(root / "vendor-b" / "Beta.vst3" / "Contents");
    std::filesystem::create_directories(root / "not-a-plugin");
    std::ofstream(root / "vendor-a" / "Alpha.vst3" / "Contents" / "moduleinfo.json") << "{}";

    std::string warning;
    const auto found = vst_host::audio::discover_vst3_plugins({root.string()}, &warning);
    assert(found.size() == 2);
    assert(found[0].find("Alpha.vst3") != std::string::npos);
    assert(found[1].find("Beta.vst3") != std::string::npos);

    const auto direct = vst_host::audio::discover_vst3_plugins(
        {(root / "vendor-a" / "Alpha.vst3").string()});
    assert(direct.size() == 1);
    assert(direct.front().find("Alpha.vst3") != std::string::npos);

    std::filesystem::remove_all(root);
    return 0;
}
