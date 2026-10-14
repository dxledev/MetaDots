#pragma once

#include <filesystem>

namespace metadots {

// GTK always loads $XDG_CONFIG_HOME/gtk-4.0/gtk.css (and gtk-dark.css) into every app and offers no
// switch to refuse it. This creates a private directory under `parent` that mirrors `config_home`
// through symlinks, except those two files, so GTK can be pointed at it with XDG_CONFIG_HOME.
// The caller owns the returned directory and removes it with std::filesystem::remove_all, which
// deletes the links without following them.
std::filesystem::path make_gtk_config_view(const std::filesystem::path& config_home,
                                           const std::filesystem::path& parent);

// Removes views in `parent` whose owning process no longer exists, such as after a crash.
void remove_stale_gtk_config_views(const std::filesystem::path& parent);

}
