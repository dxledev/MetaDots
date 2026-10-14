#include "core/gtk_config.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

static void touch(const fs::path& path, const char* text = "x") {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("metadots-gtk-config-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const auto config = root / "config";
    touch(config / "gtk-4.0/gtk.css", "* { background: red; }");
    touch(config / "gtk-4.0/gtk-dark.css");
    touch(config / "gtk-4.0/settings.ini", "[Settings]");
    touch(config / "gtk-3.0/gtk.css");
    touch(config / "metadots/theme.json", "{}");
    fs::create_directories(root / "run");

    const auto view = metadots::make_gtk_config_view(config, root / "run");
    assert(view.parent_path() == root / "run");
    // User CSS is hidden at the GTK 4 level only; everything else is reachable through links.
    assert(!fs::exists(view / "gtk-4.0/gtk.css"));
    assert(!fs::exists(view / "gtk-4.0/gtk-dark.css"));
    assert(fs::is_symlink(view / "gtk-4.0/settings.ini"));
    assert(fs::exists(view / "gtk-4.0/settings.ini"));
    assert(fs::exists(view / "gtk-3.0/gtk.css"));
    assert(fs::exists(view / "metadots/theme.json"));
    // A write through the view lands in the real config directory.
    std::ofstream(view / "metadots/theme.json") << "{\"a\":1}";
    std::ifstream written(config / "metadots/theme.json");
    std::string text;
    std::getline(written, text);
    assert(text == "{\"a\":1}");

    // Removing the view never touches the real files.
    fs::remove_all(view);
    assert(fs::exists(config / "gtk-4.0/gtk.css"));
    assert(fs::exists(config / "gtk-4.0/settings.ini"));
    assert(fs::exists(config / "metadots/theme.json"));

    // A missing config directory is fine.
    const auto empty_view = metadots::make_gtk_config_view(root / "missing", root / "run");
    assert(fs::exists(empty_view) && !fs::exists(empty_view / "gtk-4.0"));
    fs::remove_all(empty_view);

    // Views owned by dead processes are swept; live and unrelated directories stay.
    fs::create_directories(root / "run/metadots-gtk-2147483646-abcdef");
    fs::create_directories(root / "run/metadots-gtk-not-a-pid");
    fs::create_directories(root / "run/metadots-gtk-99999999999999999999-x");
    const auto live = metadots::make_gtk_config_view(config, root / "run");
    metadots::remove_stale_gtk_config_views(root / "run");
    assert(!fs::exists(root / "run/metadots-gtk-2147483646-abcdef"));
    assert(fs::exists(root / "run/metadots-gtk-not-a-pid"));
    assert(fs::exists(root / "run/metadots-gtk-99999999999999999999-x"));
    assert(fs::exists(live));
    fs::remove_all(live);

    fs::remove_all(root);
    return 0;
}
