#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace metadots {

inline constexpr std::size_t default_scan_limit = 10000;

inline constexpr std::size_t max_cover_bytes = 16 * 1024 * 1024;

// An embedded picture. Only JPEG and PNG are written, because every tag format can carry those.
struct Cover {
    std::string mime;
    std::vector<std::uint8_t> data;
};

// A pending change to a file's cover: `image` replaces it, and a null `image` removes it.
struct CoverChange {
    std::shared_ptr<const Cover> image;
};

// Returns "image/jpeg" or "image/png" from the leading bytes, or an empty string for anything else.
std::string detect_image_mime(const std::vector<std::uint8_t>& data);

// Reads and validates an image file for use as a cover. Throws if it is missing, too large, or not JPEG/PNG.
Cover load_cover_file(const std::filesystem::path& path);

// Content hash used to compare covers without keeping their bytes in memory.
std::uint64_t cover_fingerprint(const std::vector<std::uint8_t>& data);

struct Track {
    std::filesystem::path path;
    std::map<std::string, std::string> tags;
    int duration_seconds{};
    int bitrate_kbps{};
    int sample_rate{};
    std::string format;
    // Fingerprint and size of the cover shown for this file; 0 means no cover. The bytes come from read_cover().
    std::uint64_t cover_hash{};
    std::size_t cover_size{};

    struct SourceSnapshot {
        std::uint64_t device{};
        std::uint64_t inode{};
        std::uint64_t link_count{};
        std::uint64_t size{};
        std::int64_t modified_seconds{};
        std::int64_t modified_nanoseconds{};
        std::int64_t changed_seconds{};
        std::int64_t changed_nanoseconds{};
        std::uint32_t mode{};
        std::uint64_t owner{};
        std::uint64_t group{};
        bool valid{};
    };

private:
    SourceSnapshot source_snapshot_;

    friend Track read_track(const std::filesystem::path& path);
    friend Track save_tags(const Track& original,
                           const std::map<std::string, std::string>& changed,
                           const CoverChange* cover);
};

std::vector<std::filesystem::path> scan_audio(
    const std::filesystem::path& directory,
    bool recursive = false,
    std::size_t limit = default_scan_limit);

Track read_track(const std::filesystem::path& path);

// The cover of a file: its first Front Cover picture, or its first picture when none is marked front.
std::optional<Cover> read_cover(const std::filesystem::path& path);

// Writes tag changes and, when `cover` is given, replaces or removes the cover. Other pictures are kept.
Track save_tags(const Track& original,
                const std::map<std::string, std::string>& changed,
                const CoverChange* cover = nullptr);

}
