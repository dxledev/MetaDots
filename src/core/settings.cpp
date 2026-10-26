#include "settings.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>

namespace metadots {

std::filesystem::path settings_path(const std::filesystem::path& config) {
    return config.parent_path() / "settings.json";
}

Settings load_settings(const std::filesystem::path& path) {
    Settings settings;
    std::ifstream input(path);
    if (!input) return settings;
    const auto json = nlohmann::json::parse(input, nullptr, false);
    if (json.is_object()) {
        const auto found = json.find("use_native_picker");
        if (found != json.end() && found->is_boolean()) settings.use_native_picker = found->get<bool>();
    }
    return settings;
}

void save_settings(const std::filesystem::path& path, const Settings& settings) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << nlohmann::json{{"use_native_picker", settings.use_native_picker}}.dump(2) << '\n';
        if (!output.flush()) throw std::runtime_error("Settings could not be saved to " + path.string());
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("Settings could not be saved to " + path.string());
    }
}

}
