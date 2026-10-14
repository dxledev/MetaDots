#include "cli.h"

#include "core/theme.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <system_error>

namespace metadots {
namespace {

using Json = nlohmann::json;

std::vector<std::string> color_names() {
    return {"background", "surface", "surface_alt", "text", "muted", "accent",
            "accent_text", "border", "selection", "error"};
}

std::string json_output(const Theme& theme) {
    Json colors = Json::object();
    for (const auto& [key, value] : theme.colors)
        colors[key] = normalize_color(value);
    return Json{
        {"version", 1},
        {"name", theme.name},
        {"colors", std::move(colors)},
        {"source_kind", theme.source_kind},
        {"source_path", theme.source_path}
    }.dump(2) + "\n";
}

bool path_present(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return false;
    if (error)
        throw std::runtime_error("Cannot inspect theme file: " + path.string() + ": " + error.message());
    return std::filesystem::exists(status) || std::filesystem::is_symlink(status);
}

std::filesystem::path config_file_path(const std::string& value) {
    if (value.empty())
        throw std::invalid_argument("--config requires a file path");
    if (value == "~" || value.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (!home || !*home)
            throw std::runtime_error("HOME is not set");
        return std::filesystem::path(home) / (value == "~" ? "" : value.substr(2));
    }
    const std::filesystem::path path(value);
    return path.is_absolute() ? path.lexically_normal() : std::filesystem::absolute(path).lexically_normal();
}

void apply_assignments(Theme& theme, const std::map<std::string, std::string>& assignments) {
    for (const auto& [key, value] : assignments)
        theme.colors[key] = value;
    theme.name = "Custom";
    theme.source_kind.clear();
    theme.source_path.clear();
}

void print_usage(std::ostream& output) {
    output << "MetaDots — Linux audio metadata editor\n"
              "Usage: metadots [--config FILE] [FILES OR FOLDERS...]\n"
              "       metadots theme <show|set|import|follow|sync|path> [options]\n"
              "  show [--config FILE]\n"
              "  set KEY=COLOR... [--config FILE] [--dry-run]\n"
              "  import FILE [--config FILE] [--dry-run]\n"
              "  follow file PATH [--config FILE] [--dry-run]\n"
              "  follow SOURCE [PATH] [--config FILE] [--dry-run]\n"
              "  sync [--config FILE] [--dry-run]\n"
              "  path [--config FILE]\n";
}

}

int run_cli(const std::vector<std::string>& raw_args) {
    if (raw_args.empty())
        return -1;
    try {
        std::vector<std::string> args = raw_args;
        if (!args.empty() && std::filesystem::path(args.front()).filename() == "metadots")
            args.erase(args.begin());
        if (!args.empty() && args.front() == "theme")
            args.erase(args.begin());
        if (args.empty()) {
            print_usage(std::cerr);
            return 2;
        }

        bool dry_run = false;
        std::filesystem::path config = theme_path();
        std::vector<std::string> command_args;
        for (std::size_t index = 0; index < args.size(); ++index) {
            if (args[index] == "--dry-run") {
                dry_run = true;
            } else if (args[index] == "--config") {
                if (++index >= args.size())
                    throw std::invalid_argument("--config requires a file path");
                config = config_file_path(args[index]);
            } else if (args[index] == "--help" || args[index] == "-h") {
                print_usage(std::cout);
                return 0;
            } else {
                command_args.push_back(args[index]);
            }
        }
        if (!command_args.empty() && command_args.front() == "theme")
            command_args.erase(command_args.begin());
        if (command_args.empty())
            throw std::invalid_argument("A theme command is required");

        const auto command = command_args.front();
        command_args.erase(command_args.begin());
        if (command == "path") {
            if (!command_args.empty())
                throw std::invalid_argument("path does not accept arguments");
            std::cout << config.string() << '\n';
            return 0;
        }
        if (command == "show") {
            if (!command_args.empty())
                throw std::invalid_argument("show does not accept arguments");
            const auto theme = path_present(config) ? load_theme(config) : default_theme();
            std::cout << json_output(theme);
            return 0;
        }
        if (command == "set") {
            if (command_args.empty())
                throw std::invalid_argument("set requires KEY=COLOR arguments");
            const auto valid_names = color_names();
            std::map<std::string, std::string> assignments;
            for (const auto& assignment : command_args) {
                const auto equals = assignment.find('=');
                if (equals == std::string::npos || equals == 0 || equals + 1 == assignment.size())
                    throw std::invalid_argument("Expected KEY=COLOR: " + assignment);
                const auto key = assignment.substr(0, equals);
                if (std::find(valid_names.begin(), valid_names.end(), key) == valid_names.end())
                    throw std::invalid_argument("Unknown theme color: " + key);
                assignments[key] = normalize_color(assignment.substr(equals + 1));
            }

            Theme theme;
            if (dry_run) {
                theme = path_present(config) ? load_theme(config) : default_theme();
                apply_assignments(theme, assignments);
            } else {
                theme = update_theme(config, [&](Theme& current) { apply_assignments(current, assignments); });
            }
            std::cout << json_output(theme);
            return 0;
        }
        if (command == "import") {
            if (command_args.size() != 1)
                throw std::invalid_argument("import requires one palette file");
            auto theme = import_theme(command_args.front());
            if (!dry_run)
                save_theme(config, theme);
            std::cout << json_output(theme);
            return 0;
        }
        if (command == "follow") {
            if (command_args.empty() || command_args.size() > 2)
                throw std::invalid_argument("follow requires a source kind and optional path");
            const auto& kind = command_args.front();
            const auto source = command_args.size() == 2 ? command_args[1] : std::string{};
            auto theme = resolve_source(kind, source);
            if (!dry_run)
                save_theme(config, theme);
            std::cout << json_output(theme);
            return 0;
        }
        if (command == "sync") {
            if (!command_args.empty())
                throw std::invalid_argument("sync does not accept arguments");
            auto theme = load_theme(config);
            if (!theme.source_kind.empty()) {
                if (dry_run)
                    theme = refresh_theme(theme);
                else
                    theme = sync_theme(config);
            }
            std::cout << json_output(theme);
            return 0;
        }
        throw std::invalid_argument("Unknown theme command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "metadots: " << error.what() << '\n';
        return 2;
    }
}

}
