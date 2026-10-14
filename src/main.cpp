#include "cli.h"
#include "core/theme.h"
#include "ui/window.h"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        std::vector<std::string> arguments(argv + 1, argv + argc);
        std::filesystem::path config = metadots::theme_path();
        for (auto iterator = arguments.begin(); iterator != arguments.end();) {
            if (*iterator == "--") break;
            if (*iterator != "--config") { ++iterator; continue; }
            const auto next = std::next(iterator);
            if (next == arguments.end() || next->empty()) throw std::invalid_argument("--config requires a JSON file path");
            config = std::filesystem::absolute(*next);
            iterator = arguments.erase(iterator, std::next(next));
        }
        if (!arguments.empty() && arguments.front() == "--version") {
            std::cout << "MetaDots 0.1.0\n";
            return 0;
        }
        if (!arguments.empty() && (arguments.front() == "theme" || arguments.front() == "--help" || arguments.front() == "-h")) {
            arguments.insert(arguments.end(), {"--config", config.string()});
            return metadots::run_cli(arguments);
        }
        bool self_test = false;
        std::filesystem::path screenshots;
        std::vector<std::filesystem::path> paths;
        bool literal = false;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto& argument = arguments[index];
            if (!literal && argument == "--") { literal = true; continue; }
            if (!literal && argument == "--ui-self-test") { self_test = true; continue; }
            if (!literal && argument == "--screenshots") {
                if (++index >= arguments.size()) throw std::invalid_argument("--screenshots requires a directory");
                screenshots = arguments[index];
                continue;
            }
            if (!literal && argument.starts_with('-')) throw std::invalid_argument("Unknown option: " + argument);
            paths.emplace_back(argument);
        }
        return metadots::run_window(config, paths, self_test, screenshots);
    } catch (const std::exception& error) {
        std::cerr << "MetaDots: " << error.what() << '\n';
        return 1;
    }
}
