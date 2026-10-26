#include "core/settings.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() / ("metadots-settings-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const auto path = metadots::settings_path(root / "theme.json");
    assert(path == root / "settings.json");
    assert(metadots::load_settings(path).use_native_picker);
    metadots::save_settings(path, {false});
    assert(!metadots::load_settings(path).use_native_picker);
    metadots::save_settings(path, {true});
    assert(metadots::load_settings(path).use_native_picker);
    std::ofstream(path) << "not json";
    assert(metadots::load_settings(path).use_native_picker);
    std::ofstream(path) << R"({"use_native_picker": "no"})";
    assert(metadots::load_settings(path).use_native_picker);
    fs::remove_all(root);
}
