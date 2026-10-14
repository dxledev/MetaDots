#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace metadots {

namespace fs = std::filesystem;

struct Theme {
    std::string name;
    std::map<std::string, std::string> colors;
    std::string source_kind;
    std::string source_path;
};

fs::path config_home();
fs::path theme_path();
Theme default_theme();
Theme load_theme(const fs::path& path);
void ensure_theme(const fs::path& path);
Theme import_theme(const fs::path& path);
Theme resolve_source(const std::string& kind, const std::string& path = {});
Theme refresh_theme(Theme theme);
Theme sync_theme(const fs::path& path);
bool save_theme(const fs::path& path, const Theme& theme);
std::string color_css(const std::string& color);
std::string theme_css(const Theme& theme);
std::string normalize_color(const std::string& color);
Theme update_theme(const fs::path& path, const std::function<void(Theme&)>& update);

}
