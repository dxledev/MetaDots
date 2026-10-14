#include "core/theme.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

class TempDirectory {
public:
    TempDirectory() {
        std::string pattern = "/tmp/metadots-theme-test.XXXXXX";
        path_ = ::mkdtemp(pattern.data());
        if (path_.empty())
            throw std::runtime_error("Cannot create test directory");
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
    if (!output)
        throw std::runtime_error("Cannot write test file: " + path.string());
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string qml(std::string_view background, std::string_view accent) {
    return "pragma Singleton\n"
           "Singleton {\n"
           "  // property color accent: \"#000000\"\n"
           "  readonly property color background: \"" + std::string(background) + "\"\n"
           "  readonly property color foreground: \"#f0e4d1\"\n"
           "  readonly property color primary: \"" + std::string(accent) + "\"\n"
           "  readonly property color border: \"#809d9e\"\n"
           "  readonly property color backgroundAlpha: Qt.rgba(14 / 255, 30 / 255, 54 / 255, 0.88)\n"
           "}\n";
}

void retarget(const fs::path& link, const std::string& target) {
    fs::remove(link);
    fs::create_symlink(target, link);
}

void assert_accent(const metadots::Theme& theme, const std::string& expected) {
    assert(theme.colors.at("accent") == expected);
}

}

int main() {
    using namespace metadots;

    assert(normalize_color("#e00e1e36") == "#e00e1e36");
    assert(color_css("#e00e1e36") == "rgba(14, 30, 54, 0.878)");
    assert(normalize_color("Qt.rgba(14 / 255, 30 / 255, 54 / 255, 0.88)") == "#e00e1e36");
    assert(color_css("#0e1e36") == "#0e1e36");

    TempDirectory temporary;
    const auto root = temporary.path();
    setenv("HOME", root.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "xdg-config").c_str(), 1);
    setenv("XDG_STATE_HOME", (root / "xdg-state").c_str(), 1);
    const auto config = root / "metadots/theme.json";
    const auto themes = config_home() / "themes";
    const auto selector = themes / ".caelestia-use";
    fs::create_directories(themes / "first");
    fs::create_directories(themes / "second");
    fs::create_directories(themes / ".dynamic/caelestia");
    fs::create_directories(themes / ".dynamic/noctalia");
    write_file(themes / "first/Colors.qml", qml("#101318", "#e37b66"));
    write_file(themes / "second/Colors.qml", qml("#20242a", "#8fb8f5"));
    write_file(themes / ".dynamic/caelestia/Colors.qml", qml("#0a191c", "#8eb1dd"));
    write_file(themes / ".dynamic/noctalia/Colors.qml", qml("#181215", "#f7b2df"));
    const auto current = themes / "current";
    fs::create_symlink("first", current);
    fs::create_symlink("current", selector);

    auto followed = resolve_source("desktop");
    assert(followed.source_kind == "desktop");
    assert(followed.source_path == (selector / "Colors.qml").string());
    assert(followed.colors.at("background") == "#101318");
    assert(followed.colors.at("accent") == "#e37b66");
    save_theme(config, followed);

    retarget(selector, ".dynamic/caelestia");
    auto refreshed = sync_theme(config);
    assert_accent(refreshed, "#8eb1dd");
    retarget(selector, ".dynamic/noctalia");
    refreshed = sync_theme(config);
    assert_accent(refreshed, "#f7b2df");
    retarget(selector, "current");
    refreshed = sync_theme(config);
    assert_accent(refreshed, "#e37b66");
    retarget(selector, ".dynamic/caelestia");
    refreshed = sync_theme(config);
    assert_accent(refreshed, "#8eb1dd");
    const auto caelestia_palette = themes / ".dynamic/caelestia/Colors.qml";
    write_file(caelestia_palette.string() + ".next", qml("#0a191c", "#c4a8df"));
    fs::rename(caelestia_palette.string() + ".next", caelestia_palette);
    refreshed = sync_theme(config);
    assert_accent(refreshed, "#c4a8df");
    assert_accent(load_theme(config), "#c4a8df");
    retarget(selector, "current");
    assert_accent(sync_theme(config), "#e37b66");
    retarget(selector, ".dynamic/noctalia");
    assert_accent(sync_theme(config), "#f7b2df");
    const auto noctalia_palette = themes / ".dynamic/noctalia/Colors.qml";
    write_file(noctalia_palette.string() + ".next", qml("#181215", "#a7c5d9"));
    fs::rename(noctalia_palette.string() + ".next", noctalia_palette);
    assert_accent(sync_theme(config), "#a7c5d9");

    const auto static_config = root / "static-theme.json";
    auto static_theme = resolve_source("static");
    assert(static_theme.source_path == (current / "Colors.qml").string());
    assert(static_theme.colors.at("accent") == "#e37b66");
    save_theme(static_config, static_theme);
    retarget(current, "second");
    static_theme = sync_theme(static_config);
    assert_accent(static_theme, "#8fb8f5");

    retarget(selector, "current");
    refreshed = sync_theme(config);
    assert_accent(refreshed, "#8fb8f5");

    const auto state_palette = root / "xdg-state/caelestia/shell-theme-palette.json";
    write_file(state_palette,
        R"({"mode":"dark","colours":{"background":"0a191c","foreground":"ddcbb1","accent":"8eb1dd","muted":"5e6567"}})");
    const auto caelestia_source = resolve_source("caelestia");
    assert(caelestia_source.source_path == state_palette.string());
    assert(caelestia_source.colors.at("background") == "#0a191c");

    const auto noctalia_file = themes / ".dynamic/noctalia/Colors.qml";
    const auto noctalia_source = resolve_source("noctalia");
    assert(noctalia_source.source_path == noctalia_file.string());
    assert(noctalia_source.colors.at("accent") == "#a7c5d9");

    const auto last_valid = read_file(config);
    write_file(themes / "second/Colors.qml", "not a palette\n");
    const auto preserved = sync_theme(config);
    assert(preserved.colors.at("accent") == "#8fb8f5");
    assert(read_file(config) == last_valid);

    const auto bad_config = root / "bad-theme.json";
    write_file(bad_config, "{broken");
    bool rejected = false;
    try {
        ensure_theme(bad_config);
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
    assert(read_file(bad_config) == "{broken");

    const auto fifo = root / "palette.fifo";
    if (::mkfifo(fifo.c_str(), 0600) != 0)
        throw std::runtime_error("Cannot create palette FIFO");
    rejected = false;
    try {
        (void)import_theme(fifo);
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);

    write_file(root / "caelestia.json",
        R"({"mode":"dark","colours":{"background":"0a191c","foreground":"ddcbb1","accent":"8eb1dd","muted":"5e6567"}})");
    const auto caelestia = import_theme(root / "caelestia.json");
    assert(caelestia.colors.at("background") == "#0a191c");
    assert(caelestia.colors.at("accent") == "#8eb1dd");

    write_file(root / "alpha.qml",
        "Singleton {\n"
        "  readonly property color background: Qt.rgba(14 / 255, 30 / 255, 54 / 255, 0.88)\n"
        "  readonly property color foreground: \"#f0e4d1\"\n"
        "  readonly property color primary: \"#8eb1dd\"\n"
        "}\n");
    const auto alpha_palette = import_theme(root / "alpha.qml");
    assert(alpha_palette.colors.at("background") == "#e00e1e36");

    const auto css = theme_css(refreshed);
    assert(css.find("--surface-alt:") != std::string::npos);
    assert(css.find("--accent:") != std::string::npos);

    auto exported = default_theme();
    exported.name = "User Named Theme";
    const auto exported_path = root / "user-named.json";
    save_theme(exported_path, exported);
    assert(import_theme(exported_path).name == exported.name);

    assert(config_home() == root / "xdg-config");
    assert(theme_path() == root / "xdg-config/metadots/theme.json");
}
