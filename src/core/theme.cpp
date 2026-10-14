#include "theme.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace metadots {
namespace {

using Json = nlohmann::json;
constexpr std::size_t max_file_size = 1024 * 1024;

struct Rgba {
    int red = 0;
    int green = 0;
    int blue = 0;
    int alpha = 255;
};

class FileDescriptor {
public:
    explicit FileDescriptor(int value = -1) : value_(value) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    ~FileDescriptor() {
        if (value_ >= 0)
            ::close(value_);
    }

    int get() const { return value_; }

    int close() {
        const int value = std::exchange(value_, -1);
        return value >= 0 ? ::close(value) : 0;
    }

private:
    int value_;
};

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

int hex_byte(std::string_view value, std::size_t offset) {
    const int high = hex_digit(value[offset]);
    const int low = hex_digit(value[offset + 1]);
    if (high < 0 || low < 0)
        throw std::invalid_argument("Unsupported palette color: " + std::string(value));
    return high * 16 + low;
}

double channel_value(std::string expression) {
    expression = trim(expression);
    const auto slash = expression.find('/');
    if (slash != std::string::npos && expression.find('/', slash + 1) != std::string::npos)
        throw std::invalid_argument("Invalid Qt.rgba channel: " + expression);

    auto parse_number = [](const std::string& value) {
        std::size_t consumed = 0;
        double result = 0;
        try {
            result = std::stod(trim(value), &consumed);
        } catch (...) {
            throw std::invalid_argument("Invalid Qt.rgba channel: " + value);
        }
        if (consumed != trim(value).size() || !std::isfinite(result))
            throw std::invalid_argument("Invalid Qt.rgba channel: " + value);
        return result;
    };

    double result = 0;
    if (slash == std::string::npos) {
        result = parse_number(expression);
    } else {
        const double numerator = parse_number(expression.substr(0, slash));
        const double denominator = parse_number(expression.substr(slash + 1));
        if (denominator <= 0)
            throw std::invalid_argument("Invalid Qt.rgba divisor: " + expression);
        result = numerator / denominator;
    }
    if (result < 0 || result > 1)
        throw std::invalid_argument("Qt.rgba channels must be between 0 and 1");
    return result;
}

Rgba parse_color(std::string value) {
    value = trim(value);
    if (value.size() > 256)
        throw std::invalid_argument("Palette color is too long");

    constexpr std::string_view rgba_prefix = "Qt.rgba(";
    if (value.starts_with(rgba_prefix) && value.back() == ')') {
        const std::string_view arguments(value.data() + rgba_prefix.size(), value.size() - rgba_prefix.size() - 1);
        std::array<std::string, 4> parts;
        std::size_t start = 0;
        std::size_t count = 0;
        while (true) {
            const auto comma = arguments.find(',', start);
            if (count >= parts.size())
                throw std::invalid_argument("Qt.rgba needs four channels");
            parts[count++] = trim(arguments.substr(start, comma == std::string_view::npos ? arguments.size() - start : comma - start));
            if (comma == std::string_view::npos)
                break;
            start = comma + 1;
        }
        if (count != 4)
            throw std::invalid_argument("Qt.rgba needs four channels");
        return {
            static_cast<int>(std::lround(channel_value(parts[0]) * 255)),
            static_cast<int>(std::lround(channel_value(parts[1]) * 255)),
            static_cast<int>(std::lround(channel_value(parts[2]) * 255)),
            static_cast<int>(std::lround(channel_value(parts[3]) * 255))
        };
    }

    if (value == "transparent")
        return {0, 0, 0, 0};
    if (!value.empty() && value.front() == '#')
        value.erase(value.begin());
    if (value.size() == 6) {
        return {hex_byte(value, 0), hex_byte(value, 2), hex_byte(value, 4), 255};
    }
    if (value.size() == 8) {
        return {hex_byte(value, 2), hex_byte(value, 4), hex_byte(value, 6), hex_byte(value, 0)};
    }
    throw std::invalid_argument("Unsupported palette color: " + value);
}

std::string hex_color(const Rgba& color) {
    std::ostringstream output;
    output << '#';
    output << std::hex << std::nouppercase << std::setfill('0');
    if (color.alpha != 255)
        output << std::setw(2) << color.alpha;
    output << std::setw(2) << color.red << std::setw(2) << color.green << std::setw(2) << color.blue;
    return output.str();
}

std::string canonical_color(const std::string& value) {
    return hex_color(parse_color(value));
}

double linear_channel(int value) {
    const double component = static_cast<double>(value) / 255.0;
    return component <= 0.04045 ? component / 12.92 : std::pow((component + 0.055) / 1.055, 2.4);
}

double luminance(const Rgba& color) {
    return 0.2126 * linear_channel(color.red) + 0.7152 * linear_channel(color.green) + 0.0722 * linear_channel(color.blue);
}

std::string mix_color(const std::string& first, const std::string& second, double amount) {
    const auto base = parse_color(first);
    const auto tint = parse_color(second);
    const auto mix = [amount](int left, int right) {
        return static_cast<int>(std::lround(left * (1.0 - amount) + right * amount));
    };
    return hex_color({mix(base.red, tint.red), mix(base.green, tint.green), mix(base.blue, tint.blue), 255});
}

fs::path home_path() {
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home);
    throw std::runtime_error("HOME is not set");
}

fs::path xdg_path(const char* variable, const fs::path& fallback) {
    if (const char* value = std::getenv(variable); value && *value) {
        const fs::path candidate(value);
        if (candidate.is_absolute())
            return candidate.lexically_normal();
    }
    return fallback.lexically_normal();
}

fs::path expand_path(const fs::path& path) {
    const auto text = path.string();
    if (text == "~")
        return home_path();
    if (text.starts_with("~/"))
        return (home_path() / text.substr(2)).lexically_normal();
    return path.is_absolute() ? path.lexically_normal() : fs::absolute(path).lexically_normal();
}

std::string read_limited(const fs::path& path) {
    FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    const int fd = file.get();
    if (fd < 0)
        throw std::runtime_error("Cannot read palette: " + path.string() + ": " + std::strerror(errno));
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        throw std::runtime_error("Cannot inspect palette: " + path.string() + ": " + std::strerror(errno));
    }
    if (!S_ISREG(status.st_mode))
        throw std::runtime_error("Palette is not a regular file: " + path.string());
    if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > max_file_size) {
        throw std::runtime_error("Palette exceeds the 1 MiB size limit: " + path.string());
    }

    std::string result;
    result.reserve(static_cast<std::size_t>(status.st_size));
    std::array<char, 8192> buffer{};
    while (true) {
        const auto count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            throw std::runtime_error("Cannot read palette: " + path.string() + ": " + std::strerror(errno));
        }
        if (count == 0)
            break;
        if (result.size() + static_cast<std::size_t>(count) > max_file_size) {
            throw std::runtime_error("Palette exceeds the 1 MiB size limit: " + path.string());
        }
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return result;
}

class FileLock {
public:
    explicit FileLock(const fs::path& config_path) {
        const fs::path lock_path(config_path.string() + ".lock");
        fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0)
            throw std::runtime_error("Cannot open theme lock: " + lock_path.string() + ": " + std::strerror(errno));
        if (::flock(fd_, LOCK_EX) != 0) {
            const int error = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("Cannot lock theme file: " + std::string(std::strerror(error)));
        }
    }

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    ~FileLock() {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }

private:
    int fd_ = -1;
};

fs::path prepare_theme_path(const fs::path& path) {
    if (path.empty())
        throw std::invalid_argument("Theme config path cannot be empty");
    return expand_path(path);
}

std::string error_message(const std::string& operation, const fs::path& path, int error) {
    return operation + ": " + path.string() + ": " + std::strerror(error);
}

bool path_present(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return false;
    if (error)
        throw std::runtime_error("Cannot inspect theme file: " + path.string() + ": " + error.message());
    return fs::exists(status) || fs::is_symlink(status);
}

void write_atomic(const fs::path& path, std::string_view contents) {
    const auto parent = path.parent_path().empty() ? fs::current_path() : path.parent_path();
    std::error_code error;
    fs::create_directories(parent, error);
    if (error)
        throw std::runtime_error("Cannot create theme directory: " + parent.string() + ": " + error.message());

    const fs::path temporary_template = parent / (path.filename().string() + ".tmp.XXXXXX");
    std::string template_text = temporary_template.string();
    std::vector<char> temporary_name(template_text.begin(), template_text.end());
    temporary_name.push_back('\0');
    FileDescriptor file(::mkstemp(temporary_name.data()));
    const int fd = file.get();
    if (fd < 0)
        throw std::runtime_error(error_message("Cannot create temporary theme file", temporary_template, errno));
    const fs::path temporary_path(temporary_name.data());
    bool keep_temporary = true;
    try {
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const auto count = ::write(fd, contents.data() + offset, contents.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::runtime_error(error_message("Cannot write temporary theme file", temporary_path, errno));
            offset += static_cast<std::size_t>(count);
        }
        if (::fsync(fd) != 0)
            throw std::runtime_error(error_message("Cannot sync temporary theme file", temporary_path, errno));
        const int close_status = file.close();
        if (close_status != 0)
            throw std::runtime_error(error_message("Cannot close temporary theme file", temporary_path, errno));
        if (::rename(temporary_path.c_str(), path.c_str()) != 0)
            throw std::runtime_error(error_message("Cannot replace theme file", path, errno));
        keep_temporary = false;
        const int directory_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd >= 0) {
            ::fsync(directory_fd);
            ::close(directory_fd);
        }
    } catch (...) {
        if (keep_temporary)
            ::unlink(temporary_path.c_str());
        throw;
    }
}

std::vector<std::string> color_names() {
    return {"background", "surface", "surface_alt", "text", "muted", "accent",
            "accent_text", "border", "selection", "error"};
}

Theme canonical_theme(const Theme& input) {
    Theme theme = input;
    if (theme.name.empty() || theme.name.size() > 128)
        throw std::invalid_argument("Theme name must contain 1 to 128 characters");
    if (theme.source_kind.size() > 32 || theme.source_path.size() > 4096)
        throw std::invalid_argument("Theme source metadata is too long");
    if (!theme.source_kind.empty() && theme.source_kind != "desktop" && theme.source_kind != "static" &&
        theme.source_kind != "caelestia" && theme.source_kind != "noctalia" && theme.source_kind != "file")
        throw std::invalid_argument("Unknown theme source: " + theme.source_kind);

    std::map<std::string, std::string> colors;
    for (const auto& key : color_names()) {
        const auto found = theme.colors.find(key);
        if (found == theme.colors.end())
            throw std::invalid_argument("Theme is missing color: " + key);
        colors.emplace(key, canonical_color(found->second));
    }
    theme.colors = std::move(colors);
    return theme;
}

Json theme_json(const Theme& input) {
    const auto theme = canonical_theme(input);
    return Json{
        {"version", 1},
        {"name", theme.name},
        {"colors", theme.colors},
        {"source_kind", theme.source_kind},
        {"source_path", theme.source_path}
    };
}

Theme parse_theme_json(const Json& data) {
    if (!data.is_object() || data.value("version", 0) != 1)
        throw std::runtime_error("Unsupported theme file format");
    if (!data.contains("name") || !data["name"].is_string() || !data.contains("colors") || !data["colors"].is_object())
        throw std::runtime_error("Theme file is missing name or colors");

    Theme theme;
    theme.name = data["name"].get<std::string>();
    const auto& colors = data["colors"];
    for (const auto& key : color_names()) {
        const auto found = colors.find(key);
        if (found == colors.end() || !found->is_string())
            throw std::runtime_error("Theme file is missing color: " + key);
        theme.colors.emplace(key, canonical_color(found->get<std::string>()));
    }
    if (data.contains("source_kind")) {
        if (!data["source_kind"].is_string())
            throw std::runtime_error("Invalid theme source kind");
        theme.source_kind = data["source_kind"].get<std::string>();
    }
    if (data.contains("source_path")) {
        if (!data["source_path"].is_string())
            throw std::runtime_error("Invalid theme source path");
        theme.source_path = data["source_path"].get<std::string>();
    }
    return canonical_theme(theme);
}

Theme load_unlocked(const fs::path& path) {
    Json data;
    try {
        data = Json::parse(read_limited(path));
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("Invalid theme JSON in " + path.string() + ": " + error.what());
    }
    return parse_theme_json(data);
}

void save_unlocked(const fs::path& path, const Theme& theme) {
    const auto contents = theme_json(theme).dump(2) + "\n";
    write_atomic(path, contents);
}

bool is_color_value(const Json& value) {
    if (!value.is_string())
        return false;
    try {
        (void)parse_color(value.get<std::string>());
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::string> palette_color(const Json& palette, std::initializer_list<std::string_view> keys) {
    if (!palette.is_object())
        return std::nullopt;
    for (const auto key : keys) {
        const auto found = palette.find(std::string(key));
        if (found == palette.end() || !is_color_value(*found))
            continue;
        return canonical_color(found->get<std::string>());
    }
    return std::nullopt;
}

std::string require_palette_color(const Json& palette, std::initializer_list<std::string_view> keys, std::string_view display_key) {
    if (auto found = palette_color(palette, keys))
        return *found;
    throw std::runtime_error("Palette is missing " + std::string(display_key));
}

Json select_palette(Json data) {
    if (!data.is_object())
        throw std::runtime_error("Palette JSON must contain an object");
    if (data.contains("mode") && data["mode"].is_string()) {
        const auto mode = data["mode"].get<std::string>();
        if ((mode == "dark" || mode == "light") && data.contains(mode) && data[mode].is_object())
            data = data[mode];
    }
    for (const auto key : {"colors", "colours"}) {
        if (data.contains(key) && data[key].is_object())
            return data[key];
    }
    for (const auto key : {"dark", "light"}) {
        if (data.contains(key) && data[key].is_object())
            return data[key];
    }
    return data;
}

Theme map_palette(const Json& raw_palette, std::string name) {
    const Json palette = select_palette(raw_palette);
    const auto background = require_palette_color(palette, {"background", "base", "m3background", "mSurface", "surface"}, "background");
    const auto text = require_palette_color(palette, {"foreground", "onBackground", "on_background", "text", "mOnSurface", "onSurface", "on_surface"}, "text");
    const auto accent = require_palette_color(palette, {"primary", "accent", "mPrimary", "m3primary"}, "accent");
    const auto surface = palette_color(palette, {"surface", "surface_container_low", "surfaceContainerLow", "backgroundAlt", "mantle"})
        .value_or(mix_color(background, text, 0.035));
    const auto alternate = palette_color(palette, {"surface_alt", "surface_container", "surfaceContainer", "backgroundGray", "surface0", "mSurfaceVariant"})
        .value_or(mix_color(background, text, 0.07));
    const auto muted = palette_color(palette, {"muted", "foregroundInactive", "on_surface_variant", "onSurfaceVariant", "mOnSurfaceVariant"})
        .value_or(mix_color(background, text, 0.65));
    const auto accent_text = palette_color(palette, {"accent_text", "on_primary", "onPrimary", "mOnPrimary"})
        .value_or(luminance(parse_color(accent)) > 0.4 ? "#151515" : "#ffffff");
    const auto border = palette_color(palette, {"border", "outline", "mOutline"})
        .value_or(mix_color(surface, accent, 0.25));
    const auto selection = palette_color(palette, {"selection", "selectionBackground", "mSelection"})
        .value_or(mix_color(surface, accent, 0.28));
    const auto error = palette_color(palette, {"error", "danger", "red"}).value_or("#f38ba8");

    return {std::move(name), {
        {"background", background}, {"surface", surface}, {"surface_alt", alternate},
        {"text", text}, {"muted", muted}, {"accent", accent}, {"accent_text", accent_text},
        {"border", border}, {"selection", selection}, {"error", error}
    }, {}, {}};
}

struct Token {
    enum class Kind { identifier, string, punctuation } kind;
    std::string value;
};

std::vector<Token> qml_tokens(std::string_view source) {
    std::vector<Token> tokens;
    std::size_t index = 0;
    while (index < source.size()) {
        const unsigned char current = static_cast<unsigned char>(source[index]);
        if (std::isspace(current)) {
            ++index;
            continue;
        }
        if (source[index] == '/' && index + 1 < source.size() && source[index + 1] == '/') {
            index += 2;
            while (index < source.size() && source[index] != '\n')
                ++index;
            continue;
        }
        if (source[index] == '/' && index + 1 < source.size() && source[index + 1] == '*') {
            index += 2;
            while (index < source.size() && !(source[index] == '*' && index + 1 < source.size() && source[index + 1] == '/'))
                ++index;
            if (index + 1 >= source.size())
                throw std::runtime_error("Unterminated QML comment");
            index += 2;
            continue;
        }
        if (source[index] == '"' || source[index] == '\'') {
            const char quote = source[index++];
            std::string value;
            bool closed = false;
            while (index < source.size()) {
                const char character = source[index++];
                if (character == quote) {
                    closed = true;
                    break;
                }
                if (character == '\\' && index < source.size()) {
                    const char escaped = source[index++];
                    if (escaped == quote || escaped == '\\')
                        value.push_back(escaped);
                    else {
                        value.push_back('\\');
                        value.push_back(escaped);
                    }
                } else {
                    value.push_back(character);
                }
                if (value.size() > 256)
                    throw std::runtime_error("QML color string is too long");
            }
            if (!closed)
                throw std::runtime_error("Unterminated QML string");
            tokens.push_back({Token::Kind::string, std::move(value)});
            continue;
        }
        if (std::isalpha(current) || source[index] == '_') {
            const auto start = index++;
            while (index < source.size()) {
                const auto character = static_cast<unsigned char>(source[index]);
                if (!std::isalnum(character) && source[index] != '_')
                    break;
                ++index;
            }
            tokens.push_back({Token::Kind::identifier, std::string(source.substr(start, index - start))});
            continue;
        }
        tokens.push_back({Token::Kind::punctuation, std::string(1, source[index++])});
    }
    return tokens;
}

Json qml_palette(const fs::path& path) {
    const auto contents = read_limited(path);
    const auto tokens = qml_tokens(contents);
    Json palette = Json::object();
    for (std::size_t index = 0; index + 4 < tokens.size(); ++index) {
        if (tokens[index].kind != Token::Kind::identifier || tokens[index].value != "property" ||
            tokens[index + 1].kind != Token::Kind::identifier || tokens[index + 1].value != "color" ||
            tokens[index + 2].kind != Token::Kind::identifier || tokens[index + 3].value != ":")
            continue;

        const auto& key = tokens[index + 2].value;
        const auto& value = tokens[index + 4];
        if (value.kind == Token::Kind::string) {
            try {
                palette[key] = canonical_color(value.value);
            } catch (const std::invalid_argument&) {
            }
            continue;
        }
        if (value.kind == Token::Kind::identifier && value.value == "Qt" && index + 6 < tokens.size() &&
            tokens[index + 5].value == "." && tokens[index + 6].value == "rgba" && index + 7 < tokens.size() &&
            tokens[index + 7].value == "(") {
            std::string expression = "Qt.rgba(";
            std::size_t cursor = index + 8;
            while (cursor < tokens.size() && tokens[cursor].value != ")" && expression.size() < 256) {
                expression += tokens[cursor].value;
                ++cursor;
            }
            if (cursor < tokens.size() && tokens[cursor].value == ")") {
                expression += ')';
                try {
                    palette[key] = canonical_color(expression);
                } catch (const std::invalid_argument&) {
                }
            }
        }
    }
    return palette;
}

std::string source_name(const fs::path& path) {
    std::error_code error;
    auto resolved = fs::weakly_canonical(path, error);
    if (error || resolved.empty())
        resolved = path;
    auto name = resolved.parent_path().filename().string();
    if (name.empty())
        name = path.stem().string();
    return name;
}

bool is_qml_path(const fs::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension == ".qml";
}

Theme import_palette_file(const fs::path& path, const std::string& assigned_name) {
    const bool qml = is_qml_path(path);
    const auto name = assigned_name.empty() ? (qml ? source_name(path) : path.stem().string()) : assigned_name;
    if (qml)
        return map_palette(qml_palette(path), name);

    Json data;
    try {
        data = Json::parse(read_limited(path));
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("Invalid palette JSON in " + path.string() + ": " + error.what());
    }
    if (data.is_object() && data.value("version", 0) == 1 && data.contains("colors") && data["colors"].is_object()) {
        try {
            auto imported = parse_theme_json(data);
            if (!assigned_name.empty())
                imported.name = assigned_name;
            imported.source_kind.clear();
            imported.source_path.clear();
            return imported;
        } catch (const std::exception&) {
        }
    }
    return map_palette(data, name);
}

fs::path source_path(const std::string& kind, const std::string& requested) {
    const fs::path config = config_home();
    const fs::path themes = config / "themes";
    if (kind == "desktop") {
        fs::path selector = requested.empty() ? themes / ".caelestia-use" : expand_path(requested);
        if (selector.filename() == "Colors.qml")
            return selector;
        return selector / "Colors.qml";
    }
    if (kind == "static") {
        fs::path selected = requested.empty() ? fs::path("current") : fs::path(requested);
        if (!selected.is_absolute()) {
            const auto text = selected.string();
            if (text.find('/') == std::string::npos && text != "." && text != "..")
                selected = themes / selected;
            else
                selected = expand_path(selected);
        } else {
            selected = selected.lexically_normal();
        }
        std::error_code error;
        if (fs::is_directory(selected, error)) {
            const auto qml = selected / "Colors.qml";
            if (fs::exists(qml))
                return qml;
            return selected / "noctalia.json";
        }
        return selected;
    }
    if (kind == "caelestia") {
        if (!requested.empty())
            return expand_path(requested);
        const auto state = xdg_path("XDG_STATE_HOME", home_path() / ".local/state");
        return state / "caelestia/shell-theme-palette.json";
    }
    if (kind == "noctalia") {
        return requested.empty() ? themes / ".dynamic/noctalia/Colors.qml" : expand_path(requested);
    }
    if (kind == "file") {
        if (requested.empty())
            throw std::invalid_argument("The file source requires a path");
        return expand_path(requested);
    }
    throw std::invalid_argument("Unknown source. Use desktop, static, caelestia, noctalia, or file.");
}

Theme resolve_source_internal(const std::string& kind, const std::string& requested) {
    const auto path = source_path(kind, requested);
    auto theme = import_palette_file(path, kind == "desktop" || kind == "static" ? kind + ": " + source_name(path) : kind);
    theme.source_kind = kind;
    theme.source_path = path.string();
    return canonical_theme(theme);
}

bool same_theme(const Theme& first, const Theme& second) {
    return theme_json(first) == theme_json(second);
}

std::string css_alpha(int value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << static_cast<double>(value) / 255.0;
    std::string result = output.str();
    while (!result.empty() && result.back() == '0')
        result.pop_back();
    if (!result.empty() && result.back() == '.')
        result.pop_back();
    return result;
}

}

fs::path config_home() {
    return xdg_path("XDG_CONFIG_HOME", home_path() / ".config");
}

fs::path theme_path() {
    return config_home() / "metadots/theme.json";
}

Theme default_theme() {
    return {"Midnight", {
        {"background", "#111318"}, {"surface", "#191c22"}, {"surface_alt", "#22262e"},
        {"text", "#e7eaf0"}, {"muted", "#9ba2ae"}, {"accent", "#8fb8f5"},
        {"accent_text", "#111318"}, {"border", "#343a45"}, {"selection", "#36465e"},
        {"error", "#f38ba8"}
    }, {}, {}};
}

Theme load_theme(const fs::path& path) {
    return load_unlocked(prepare_theme_path(path));
}

void ensure_theme(const fs::path& requested_path) {
    const auto path = prepare_theme_path(requested_path);
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error)
        throw std::runtime_error("Cannot create theme directory: " + path.parent_path().string() + ": " + error.message());
    FileLock lock(path);
    if (path_present(path))
        (void)load_unlocked(path);
    else
        save_unlocked(path, default_theme());
}

Theme import_theme(const fs::path& requested_path) {
    return import_palette_file(prepare_theme_path(requested_path), {});
}

Theme resolve_source(const std::string& kind, const std::string& path) {
    return resolve_source_internal(kind, path);
}

Theme refresh_theme(Theme theme) {
    if (theme.source_kind.empty())
        return theme;
    return resolve_source_internal(theme.source_kind, theme.source_path);
}

Theme sync_theme(const fs::path& requested_path) {
    const auto path = prepare_theme_path(requested_path);
    FileLock lock(path);
    auto previous = load_unlocked(path);
    if (previous.source_kind.empty())
        return previous;
    try {
        auto updated = refresh_theme(previous);
        if (!same_theme(previous, updated))
            save_unlocked(path, updated);
        return updated;
    } catch (const std::exception&) {
        return previous;
    }
}

Theme update_theme(const fs::path& requested_path, const std::function<void(Theme&)>& update) {
    const auto path = prepare_theme_path(requested_path);
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error)
        throw std::runtime_error("Cannot create theme directory: " + path.parent_path().string() + ": " + error.message());
    FileLock lock(path);
    auto theme = path_present(path) ? load_unlocked(path) : default_theme();
    update(theme);
    theme = canonical_theme(theme);
    save_unlocked(path, theme);
    return theme;
}

bool save_theme(const fs::path& requested_path, const Theme& theme) {
    const auto path = prepare_theme_path(requested_path);
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error)
        throw std::runtime_error("Cannot create theme directory: " + path.parent_path().string() + ": " + error.message());
    FileLock lock(path);
    save_unlocked(path, theme);
    return true;
}

std::string normalize_color(const std::string& color) {
    return canonical_color(color);
}

std::string color_css(const std::string& value) {
    const auto color = parse_color(value);
    if (color.alpha == 255)
        return hex_color(color);
    return "rgba(" + std::to_string(color.red) + ", " + std::to_string(color.green) + ", " +
        std::to_string(color.blue) + ", " + css_alpha(color.alpha) + ")";
}

std::string theme_css(const Theme& input) {
    const auto theme = canonical_theme(input);
    std::ostringstream output;
    output << ":root {\n";
    for (const auto& [key, color] : theme.colors) {
        std::string css_key = key;
        std::replace(css_key.begin(), css_key.end(), '_', '-');
        output << "  --" << css_key << ": " << color_css(color) << ";\n";
    }
    output << "}\n";
    return output.str();
}

}
