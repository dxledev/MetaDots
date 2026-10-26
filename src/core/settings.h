#pragma once

#include <filesystem>

namespace metadots {

struct Settings {
    // On uses MetaDots' own in-window file browser; off asks the desktop's file chooser portal.
    bool use_native_picker{true};
};

// Settings live beside the theme file, in their own file, so theme reloads never touch them.
std::filesystem::path settings_path(const std::filesystem::path& config);

// A missing or unreadable file yields the defaults.
Settings load_settings(const std::filesystem::path& path);

// Throws std::runtime_error when the file cannot be written.
void save_settings(const std::filesystem::path& path, const Settings& settings);

}
