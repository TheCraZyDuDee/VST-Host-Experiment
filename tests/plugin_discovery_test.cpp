#include "audio/plugin_discovery.hpp"

#include <algorithm>
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

    // Reproduce the duplicated vstbridge layout produced by overlapping
    // registrations: Windows/VST3 and its parent Windows. Both Linux wrapper
    // trees point at precisely the same original Windows modules.
    const auto windows = root / "Windows Plugins" / "VST3";
    const auto original_rnnoise = windows / "rnnoise.vst3" / "Contents" / "x86_64-win" / "rnnoise.vst3";
    const auto original_clear = windows / "Supertone" / "Clear.vst3";
    std::filesystem::create_directories(original_rnnoise.parent_path());
    std::filesystem::create_directories(original_clear / "Contents");
    std::ofstream(original_rnnoise) << "original Windows module";

    const auto wrappers = root / ".vst3" / "vstbridge";
    auto make_wrapper = [&](const std::filesystem::path& name,
                            const std::filesystem::path& original_module,
                            const std::string& module_name) {
        const auto wrapper = wrappers / name;
        const auto win = wrapper / "Contents" / "x86_64-win";
        std::filesystem::create_directories(win);
        std::filesystem::create_symlink(original_module, win / module_name);
        return wrapper;
    };

    const auto rnnoise_short = make_wrapper(
        "rnnoise.vst3", original_rnnoise, "rnnoise.vst3");
    const auto rnnoise_nested = make_wrapper(
        "VST3/rnnoise.vst3", original_rnnoise, "rnnoise.vst3");
    const auto clear_short = make_wrapper(
        "Supertone/Clear.vst3", original_clear, "Clear.vst3");
    const auto clear_nested = make_wrapper(
        "VST3/Supertone/Clear.vst3", original_clear, "Clear.vst3");

    // Same display name but different original Windows module is NOT a
    // duplicate. This must remain selectable as a separate effect.
    const auto other_module = root / "Another Prefix" / "rnnoise.vst3";
    std::filesystem::create_directories(other_module.parent_path());
    std::ofstream(other_module) << "different module";
    const auto other_wrapper = make_wrapper(
        "Other/rnnoise.vst3", other_module, "rnnoise.vst3");

    const auto detected = vst_host::audio::discover_vst3_plugins({wrappers.string()});
    if (detected.size() != 5) return 4;
    if (vst_host::audio::vst3_plugin_source_identity(rnnoise_short.string()) !=
        vst_host::audio::vst3_plugin_source_identity(rnnoise_nested.string())) return 5;
    if (vst_host::audio::vst3_plugin_source_identity(clear_short.string()) !=
        vst_host::audio::vst3_plugin_source_identity(clear_nested.string())) return 6;
    if (vst_host::audio::vst3_plugin_source_identity(rnnoise_short.string()) ==
        vst_host::audio::vst3_plugin_source_identity(other_wrapper.string())) return 7;

    std::size_t discarded = 0;
    const auto unique = vst_host::audio::deduplicate_discovered_vst3_plugins(
        detected, &discarded);
    if (discarded != 2 || unique.size() != 3) return 8;
    if (std::find(unique.begin(), unique.end(), rnnoise_short.string()) == unique.end() ||
        std::find(unique.begin(), unique.end(), clear_short.string()) == unique.end() ||
        std::find(unique.begin(), unique.end(), other_wrapper.string()) == unique.end())
        return 9;

    // The GUI list also receives active chain entries and native directory
    // discoveries. The same original plugin must still appear only once.
    std::vector<std::pair<std::string, bool>> browser = {
        {rnnoise_nested.string(), true}, // previously selected chain
        {rnnoise_short.string(), true},
        {clear_nested.string(), false},  // wrapper under a Linux scan root
        {clear_short.string(), true},
        {other_wrapper.string(), true},
        {rnnoise_short.string(), true},
    };
    auto entries = vst_host::audio::deduplicate_vst3_browser_entries(
        browser, &discarded);
    if (entries.size() != 3 || discarded != 3) return 10;
    for (const auto& [path, wine] : entries)
        if (!wine) return 11;
    if (std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.first == rnnoise_short.string();
        }) == entries.end()) return 12;

    // Native plugins with identical names remain distinct when their actual
    // bundle roots differ. No name-only deduplication is ever performed.
    const auto native_a = root / "vendor-a" / "Alpha.vst3";
    const auto native_b = root / "vendor-b" / "Alpha.vst3";
    std::filesystem::create_directories(native_b);
    auto native = vst_host::audio::deduplicate_discovered_vst3_plugins(
        {native_a.string(), native_b.string()});
    if (native.size() != 2) return 13;

    std::filesystem::remove_all(root);
    return 0;
}
