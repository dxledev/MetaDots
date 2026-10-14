#include "gtk_config.h"

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

namespace metadots {
namespace fs = std::filesystem;
namespace {

constexpr const char* gtk_directory = "gtk-4.0";
constexpr const char* view_prefix = "metadots-gtk-";

bool hidden_gtk_file(const std::string& name) {
    return name == "gtk.css" || name == "gtk-dark.css";
}

void link_entries(const fs::path& source, const fs::path& target, bool gtk_level) {
    std::error_code error;
    if (!fs::is_directory(source, error)) return;
    for (fs::directory_iterator it(source, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (gtk_level ? hidden_gtk_file(name) : name == gtk_directory) continue;
        std::error_code link_error;
        fs::create_symlink(it->path(), target / name, link_error);
        if (link_error)
            throw std::runtime_error("Cannot link " + it->path().string() + ": " + link_error.message());
    }
    if (error) throw std::runtime_error("Cannot read " + source.string() + ": " + error.message());
}

}

fs::path make_gtk_config_view(const fs::path& config_home, const fs::path& parent) {
    std::string pattern = (parent / (std::string(view_prefix) + std::to_string(::getpid()) + "-XXXXXX")).string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (!::mkdtemp(buffer.data()))
        throw std::runtime_error("Cannot create a private GTK config directory in " + parent.string());
    const fs::path view(buffer.data());
    try {
        link_entries(config_home, view, false);
        const auto gtk_source = config_home / gtk_directory;
        std::error_code error;
        if (fs::is_directory(gtk_source, error)) {
            fs::create_directory(view / gtk_directory);
            link_entries(gtk_source, view / gtk_directory, true);
        }
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(view, ignored);
        throw;
    }
    return view;
}

void remove_stale_gtk_config_views(const fs::path& parent) {
    std::error_code error;
    for (fs::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!name.starts_with(view_prefix) || !it->is_directory(error) || it->is_symlink(error)) continue;
        const auto digits = name.substr(std::string(view_prefix).size(), name.find('-', std::string(view_prefix).size()) - std::string(view_prefix).size());
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
        long owner = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), owner);
        if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size()) continue;
        if (owner > 0 && ::kill(static_cast<pid_t>(owner), 0) == -1 && errno == ESRCH) {
            std::error_code ignored;
            fs::remove_all(it->path(), ignored);
        }
    }
}

}
