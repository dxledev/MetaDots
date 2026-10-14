#include "metadata.h"

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/id3v2header.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>
#include <taglib/tvariant.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <queue>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

#include <linux/fs.h>

namespace metadots {
namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            close();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    ~FileDescriptor() { close(); }

    int get() const noexcept { return descriptor_; }

    void close() noexcept {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }

private:
    int descriptor_;
};

class TemporaryFile {
public:
    explicit TemporaryFile(std::filesystem::path path, int descriptor)
        : path_(std::move(path)), descriptor_(descriptor) {}
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    ~TemporaryFile() {
        descriptor_.close();
        if (cleanup_path_) {
            ::unlink(path_.c_str());
        }
    }

    const std::filesystem::path& path() const noexcept { return path_; }
    int descriptor() const noexcept { return descriptor_.get(); }
    void preserve_path() noexcept { cleanup_path_ = false; }
    void allow_cleanup() noexcept { cleanup_path_ = true; }

private:
    std::filesystem::path path_;
    FileDescriptor descriptor_;
    bool cleanup_path_{true};
};

[[noreturn]] void throw_errno(std::string_view action,
                              const std::filesystem::path& path) {
    const int error = errno;
    throw std::system_error(error, std::generic_category(),
                            std::string(action) + " '" + path.string() + "'");
}

Track::SourceSnapshot snapshot_from_stat(const struct stat& status) {
    Track::SourceSnapshot snapshot;
    snapshot.device = static_cast<std::uint64_t>(status.st_dev);
    snapshot.inode = static_cast<std::uint64_t>(status.st_ino);
    snapshot.link_count = static_cast<std::uint64_t>(status.st_nlink);
    snapshot.size = static_cast<std::uint64_t>(status.st_size);
    snapshot.modified_seconds = static_cast<std::int64_t>(status.st_mtim.tv_sec);
    snapshot.modified_nanoseconds = static_cast<std::int64_t>(status.st_mtim.tv_nsec);
    snapshot.changed_seconds = static_cast<std::int64_t>(status.st_ctim.tv_sec);
    snapshot.changed_nanoseconds = static_cast<std::int64_t>(status.st_ctim.tv_nsec);
    snapshot.mode = static_cast<std::uint32_t>(status.st_mode & 07777);
    snapshot.owner = static_cast<std::uint64_t>(status.st_uid);
    snapshot.group = static_cast<std::uint64_t>(status.st_gid);
    snapshot.valid = true;
    return snapshot;
}

bool same_snapshot(const Track::SourceSnapshot& expected,
                   const Track::SourceSnapshot& actual) {
    return expected.valid && actual.valid && expected.device == actual.device &&
           expected.inode == actual.inode && expected.link_count == actual.link_count &&
           expected.size == actual.size &&
           expected.modified_seconds == actual.modified_seconds &&
           expected.modified_nanoseconds == actual.modified_nanoseconds &&
           expected.changed_seconds == actual.changed_seconds &&
           expected.changed_nanoseconds == actual.changed_nanoseconds &&
           expected.mode == actual.mode && expected.owner == actual.owner &&
           expected.group == actual.group;
}

bool same_moved_source(const Track::SourceSnapshot& expected,
                       const Track::SourceSnapshot& actual) {
    return expected.valid && actual.valid && expected.device == actual.device &&
           expected.inode == actual.inode && expected.link_count == actual.link_count &&
           expected.size == actual.size &&
           expected.modified_seconds == actual.modified_seconds &&
           expected.modified_nanoseconds == actual.modified_nanoseconds &&
           expected.mode == actual.mode && expected.owner == actual.owner &&
           expected.group == actual.group;
}

Track::SourceSnapshot stat_descriptor(int descriptor,
                                      const std::filesystem::path& path) {
    struct stat status {};
    if (::fstat(descriptor, &status) < 0) {
        throw_errno("Cannot inspect", path);
    }
    if (!S_ISREG(status.st_mode)) {
        throw std::runtime_error("Audio path is not a regular file: '" + path.string() + "'");
    }
    return snapshot_from_stat(status);
}

Track::SourceSnapshot stat_path_at(int directory_descriptor,
                                   const std::string& filename,
                                   const std::filesystem::path& path) {
    struct stat status {};
    if (::fstatat(directory_descriptor, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) < 0) {
        throw_errno("Cannot inspect", path);
    }
    if (!S_ISREG(status.st_mode)) {
        throw std::runtime_error("Audio path is not a regular file: '" + path.string() + "'");
    }
    return snapshot_from_stat(status);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string display_tag_key(std::string key) {
    key = lowercase_ascii(std::move(key));
    if (key == "albumartist") {
        return "album_artist";
    }
    if (key == "tracknumber") {
        return "track";
    }
    if (key == "discnumber") {
        return "disc";
    }
    return key;
}

std::string property_key_for_edit(const std::string& key) {
    const std::string normalized = display_tag_key(key);
    if (normalized == "title") return "TITLE";
    if (normalized == "artist") return "ARTIST";
    if (normalized == "album") return "ALBUM";
    if (normalized == "album_artist") return "ALBUMARTIST";
    if (normalized == "track") return "TRACKNUMBER";
    if (normalized == "disc") return "DISCNUMBER";
    if (normalized == "date") return "DATE";
    if (normalized == "genre") return "GENRE";
    if (normalized == "comment") return "COMMENT";
    if (normalized == "composer") return "COMPOSER";
    if (normalized == "copyright") return "COPYRIGHT";
    throw std::invalid_argument("Unsupported editable tag key: '" + key + "'");
}

std::string display_format(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(extension.begin());
    }
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return extension;
}

std::string join_values(const TagLib::StringList& values) {
    std::string result;
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!result.empty()) {
            result += "; ";
        }
        const std::string value = it->to8Bit(true);
        result.append(value.data(), value.size());
    }
    return result;
}


using PictureList = TagLib::List<TagLib::VariantMap>;

std::string picture_type(const TagLib::VariantMap& picture) {
    const auto found = picture.find("pictureType");
    return found == picture.end() ? std::string() : found->second.toString().to8Bit(true);
}

// MP4 pictures carry no type, so a missing type counts as the front cover.
bool is_front_picture(const TagLib::VariantMap& picture) {
    const auto type = picture_type(picture);
    return type.empty() || type == "Front Cover";
}

// The pictures MetaDots treats as "the cover": front covers and untyped or "Other" pictures.
// Back covers, leaflets, artist photos and the like are never shown, replaced or removed.
bool is_cover_picture(const TagLib::VariantMap& picture) {
    return is_front_picture(picture) || picture_type(picture) == "Other";
}

// Index of the picture shown as the cover: the first front picture, else the first "Other" one.
std::optional<std::size_t> shown_cover_index(const std::vector<TagLib::VariantMap>& pictures) {
    for (std::size_t index = 0; index < pictures.size(); ++index)
        if (is_front_picture(pictures[index])) return index;
    for (std::size_t index = 0; index < pictures.size(); ++index)
        if (is_cover_picture(pictures[index])) return index;
    return std::nullopt;
}

std::vector<TagLib::VariantMap> to_vector(const PictureList& pictures) {
    return std::vector<TagLib::VariantMap>(pictures.begin(), pictures.end());
}

std::vector<std::uint8_t> picture_bytes(const TagLib::VariantMap& picture) {
    const auto found = picture.find("data");
    if (found == picture.end()) return {};
    const TagLib::ByteVector data = found->second.toByteVector();
    return std::vector<std::uint8_t>(data.begin(), data.end());
}

TagLib::VariantMap make_front_cover(const Cover& cover) {
    TagLib::VariantMap picture;
    picture["data"] = TagLib::ByteVector(reinterpret_cast<const char*>(cover.data.data()),
                                         static_cast<unsigned int>(cover.data.size()));
    picture["mimeType"] = TagLib::String(cover.mime);
    picture["pictureType"] = TagLib::String("Front Cover");
    picture["description"] = TagLib::String();
    return picture;
}

// Replaces or removes the cover pictures and keeps every other picture in place.
std::vector<TagLib::VariantMap> edited_pictures(const std::vector<TagLib::VariantMap>& pictures,
                                                const CoverChange& change) {
    std::vector<TagLib::VariantMap> result;
    bool inserted = false;
    for (const auto& picture : pictures) {
        if (!is_cover_picture(picture)) { result.push_back(picture); continue; }
        if (change.image && !inserted) { result.push_back(make_front_cover(*change.image)); inserted = true; }
    }
    if (change.image && !inserted) result.insert(result.begin(), make_front_cover(*change.image));
    return result;
}

// Pictures show up in a file's "unsupported" tag data under format-specific names, so a cover edit
// legitimately changes those entries.
std::vector<std::string> comparable_unsupported(const TagLib::StringList& entries, bool ignore_pictures) {
    static constexpr std::array<std::string_view, 6> picture_prefixes{
        "APIC", "PIC", "COVR", "WM/PICTURE", "METADATA_BLOCK_PICTURE", "COVER ART"};
    std::vector<std::string> result;
    for (const auto& entry : entries) {
        std::string text = entry.to8Bit(true);
        const std::string upper = [&] { std::string copy = text; for (auto& c : copy) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return copy; }();
        const bool picture = std::any_of(picture_prefixes.begin(), picture_prefixes.end(),
                                         [&](std::string_view prefix) { return upper.starts_with(prefix); });
        if (!(ignore_pictures && picture)) result.push_back(std::move(text));
    }
    return result;
}

using ComplexProperties = std::vector<std::pair<TagLib::String, PictureList>>;

Track read_track_impl(const std::filesystem::path& path,
                      Track::SourceSnapshot& snapshot_output) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        throw_errno("Cannot open", path);
    }
    FileDescriptor source(descriptor);
    const Track::SourceSnapshot initial_snapshot = stat_descriptor(source.get(), path);

    Track track;
    track.path = path;
    track.format = display_format(path);

    {
        TagLib::FileRef file(path.c_str(), true, TagLib::AudioProperties::Fast);
        if (file.isNull() || file.file() == nullptr || !file.file()->isValid()) {
            throw std::runtime_error("Unsupported or malformed audio file: '" + path.string() + "'");
        }
        const auto* audio = file.audioProperties();
        if (audio == nullptr || audio->sampleRate() <= 0) {
            throw std::runtime_error("Audio file has no valid audio properties: '" + path.string() + "'");
        }

        const TagLib::PropertyMap properties = file.properties();
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            const std::string key = display_tag_key(it->first.to8Bit(true));
            track.tags[key] = join_values(it->second);
        }

        const auto pictures = to_vector(file.complexProperties("PICTURE"));
        if (const auto shown = shown_cover_index(pictures)) {
            const auto bytes = picture_bytes(pictures[*shown]);
            track.cover_hash = cover_fingerprint(bytes);
            track.cover_size = bytes.size();
        }

        track.duration_seconds = audio->lengthInSeconds();
        track.bitrate_kbps = audio->bitrate();
        track.sample_rate = audio->sampleRate();
    }

    const Track::SourceSnapshot descriptor_snapshot = stat_descriptor(source.get(), path);
    struct stat path_status {};
    if (::lstat(path.c_str(), &path_status) < 0) {
        throw_errno("Cannot inspect", path);
    }
    const Track::SourceSnapshot path_snapshot = snapshot_from_stat(path_status);
    if (!same_snapshot(initial_snapshot, descriptor_snapshot) ||
        !same_snapshot(initial_snapshot, path_snapshot)) {
        throw std::runtime_error("Audio file changed while it was being read: '" + path.string() + "'");
    }
    snapshot_output = initial_snapshot;
    return track;
}

bool has_audio_extension(const std::filesystem::path& path) {
    static const std::array<std::string_view, 21> extensions{
        ".aac", ".aif", ".aiff", ".ape", ".asf", ".dff", ".dsf", ".flac", ".m4a",
        ".mp2", ".mp3", ".mp4", ".mpc", ".oga", ".ogg", ".opus", ".spx", ".tta",
        ".wav", ".wma", ".wv"};
    const std::string extension = lowercase_ascii(path.extension().string());
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

struct PathGreater {
    bool operator()(const std::filesystem::path& left,
                    const std::filesystem::path& right) const {
        return left.native() < right.native();
    }
};

void write_all(int descriptor, const char* data, std::size_t size,
               const std::filesystem::path& path) {
    while (size > 0) {
        const ssize_t written = ::write(descriptor, data, size);
        if (written < 0) {
            if (errno == EINTR) continue;
            throw_errno("Cannot write temporary audio file", path);
        }
        if (written == 0) {
            throw std::runtime_error("Short write to temporary audio file: '" + path.string() + "'");
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

void copy_source(int source_descriptor, int destination_descriptor,
                 std::uint64_t expected_size, const std::filesystem::path& path) {
    std::array<char, 65536> buffer{};
    std::uint64_t offset = 0;
    while (offset < expected_size) {
        const std::size_t requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), expected_size - offset));
        const ssize_t count = ::pread(source_descriptor, buffer.data(), requested,
                                      static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) continue;
            throw_errno("Cannot read", path);
        }
        if (count == 0) {
            throw std::runtime_error("Audio file changed while it was being copied: '" + path.string() + "'");
        }
        write_all(destination_descriptor, buffer.data(), static_cast<std::size_t>(count), path);
        offset += static_cast<std::uint64_t>(count);
    }
}

std::vector<std::string> list_xattrs(int descriptor,
                                     const std::filesystem::path& path) {
    for (;;) {
        const ssize_t size = ::flistxattr(descriptor, nullptr, 0);
        if (size < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOTSUP || errno == EOPNOTSUPP) return {};
            throw_errno("Cannot inspect extended attributes on", path);
        }
        std::vector<char> names(static_cast<std::size_t>(size));
        const ssize_t count = ::flistxattr(descriptor, names.data(), names.size());
        if (count < 0) {
            if (errno == EINTR || errno == ERANGE) continue;
            if (errno == ENOTSUP || errno == EOPNOTSUPP) return {};
            throw_errno("Cannot inspect extended attributes on", path);
        }

        std::vector<std::string> result;
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(count)) {
            const std::size_t length = std::strlen(names.data() + offset);
            if (length == 0 || offset + length >= static_cast<std::size_t>(count)) {
                throw std::runtime_error("Malformed extended attribute list on '" + path.string() + "'");
            }
            result.emplace_back(names.data() + offset, length);
            offset += length + 1;
        }
        return result;
    }
}

std::vector<char> read_xattr(int descriptor, const std::string& name,
                             const std::filesystem::path& path) {
    for (;;) {
        const ssize_t size = ::fgetxattr(descriptor, name.c_str(), nullptr, 0);
        if (size < 0) {
            if (errno == EINTR || errno == ERANGE) continue;
            throw_errno("Cannot read extended attribute from", path);
        }
        std::vector<char> value(static_cast<std::size_t>(size));
        const ssize_t count = ::fgetxattr(descriptor, name.c_str(), value.data(), value.size());
        if (count < 0) {
            if (errno == EINTR || errno == ERANGE) continue;
            throw_errno("Cannot read extended attribute from", path);
        }
        value.resize(static_cast<std::size_t>(count));
        return value;
    }
}

void copy_xattrs(int source_descriptor, int destination_descriptor,
                 const std::filesystem::path& path) {
    const std::vector<std::string> source_names = list_xattrs(source_descriptor, path);
    const std::unordered_set<std::string> source_name_set(source_names.begin(), source_names.end());
    for (const std::string& name : list_xattrs(destination_descriptor, path)) {
        if (!source_name_set.contains(name) &&
            ::fremovexattr(destination_descriptor, name.c_str()) < 0 && errno != ENODATA) {
            throw_errno("Cannot remove temporary extended attribute from", path);
        }
    }

    for (const std::string& name : source_names) {
        const std::vector<char> value = read_xattr(source_descriptor, name, path);
        if (::fsetxattr(destination_descriptor, name.c_str(), value.data(), value.size(), 0) < 0) {
            throw_errno("Cannot preserve extended attribute on", path);
        }
    }
}

TemporaryFile create_temporary_file(const std::filesystem::path& directory,
                                    const std::filesystem::path& target) {
    const std::string pattern = (directory / ".metadots-save-XXXXXX").string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    const int descriptor = ::mkstemp(mutable_pattern.data());
    if (descriptor < 0) {
        throw_errno("Cannot create temporary file beside", target);
    }
    if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
        const int error = errno;
        ::close(descriptor);
        ::unlink(mutable_pattern.data());
        errno = error;
        throw_errno("Cannot secure temporary file beside", target);
    }
    const std::filesystem::path temporary_path(mutable_pattern.data());
    return TemporaryFile(temporary_path, descriptor);
}

void verify_metadata_preserved(
    const TagLib::PropertyMap& original_properties,
    const std::set<std::string>& expected_complex_keys,
    const ComplexProperties& complex_properties,
    const TagLib::StringList& original_unsupported,
    const TagLib::FileRef& actual_file,
    const std::map<std::string, std::string>& changed_properties,
    bool cover_edited,
    const std::filesystem::path& path) {
    const TagLib::PropertyMap actual_properties = actual_file.properties();
    for (auto it = original_properties.begin(); it != original_properties.end(); ++it) {
        if (changed_properties.contains(it->first.to8Bit(true))) {
            continue;
        }
        const auto actual = actual_properties.find(it->first);
        if (actual == actual_properties.end() || actual->second != it->second) {
            throw std::runtime_error("TagLib did not preserve existing tag '" +
                                     it->first.to8Bit(true) + "'");
        }
    }
    for (const auto& [property, value] : changed_properties) {
        const TagLib::String key(property, TagLib::String::UTF8);
        const auto actual = actual_properties.find(key);
        if (value.empty()) {
            if (actual != actual_properties.end()) {
                throw std::runtime_error("TagLib did not clear tag '" + property + "'");
            }
        } else if (actual == actual_properties.end() || actual->second.size() != 1 ||
                   actual->second.front().to8Bit(true) != value) {
            throw std::runtime_error("TagLib did not store tag '" + property + "' as requested");
        }
    }
    if (comparable_unsupported(original_properties.unsupportedData(), cover_edited) !=
            comparable_unsupported(actual_properties.unsupportedData(), cover_edited) ||
        comparable_unsupported(original_unsupported, cover_edited) !=
            comparable_unsupported(actual_properties.unsupportedData(), cover_edited)) {
        throw std::runtime_error("TagLib did not preserve unsupported metadata in '" + path.string() + "'");
    }
    // TagLib keeps listing a key after its last picture is removed, so only keys with content count.
    std::set<std::string> actual_complex_keys;
    for (const auto& key : actual_file.complexPropertyKeys())
        if (!actual_file.complexProperties(key).isEmpty()) actual_complex_keys.insert(key.to8Bit(true));
    if (expected_complex_keys != actual_complex_keys) {
        throw std::runtime_error("TagLib changed the available complex metadata in '" + path.string() + "'");
    }
    for (const auto& [key, properties] : complex_properties) {
        const auto actual = actual_file.complexProperties(key);
        if (cover_edited && key == "PICTURE") {
            // Formats add or drop descriptive fields (FLAC sizes, MP4 types), so compare the image bytes.
            bool same = actual.size() == properties.size();
            for (auto expected = properties.begin(), found = actual.begin();
                 same && expected != properties.end(); ++expected, ++found)
                same = picture_bytes(*expected) == picture_bytes(*found);
            if (!same) throw std::runtime_error("TagLib did not store the cover image in '" + path.string() + "'");
        } else if (actual != properties) {
            throw std::runtime_error("TagLib changed complex metadata in '" + path.string() + "'");
        }
    }
}

void update_temporary_tags(const std::filesystem::path& path,
                           const std::vector<std::pair<std::string, std::string>>& changes,
                           const CoverChange* cover) {
    TagLib::PropertyMap original_properties;
    std::set<std::string> expected_complex_keys;
    TagLib::StringList original_unsupported;
    ComplexProperties complex_properties;
    std::map<std::string, std::string> changed_properties;
    for (const auto& [key, value] : changes) {
        changed_properties[property_key_for_edit(key)] = value;
    }

    {
        TagLib::FileRef file(path.c_str(), false);
        if (file.isNull() || file.file() == nullptr || !file.file()->isValid()) {
            throw std::runtime_error("TagLib cannot reopen temporary audio file: '" + path.string() + "'");
        }

        // TagLib's default MP3 save silently upgrades ID3v2.3 to v2.4 and adds an ID3v1 tag. Players
        // and phone apps that only understand v2.3 then lose track numbers, so keep what the file had.
        auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file.file());
        TagLib::ID3v2::Version id3_version = TagLib::ID3v2::v3;
        bool had_id3v1 = false;
        if (mpeg) {
            had_id3v1 = mpeg->hasID3v1Tag();
            if (mpeg->hasID3v2Tag() && mpeg->ID3v2Tag()->header()->majorVersion() >= 4) id3_version = TagLib::ID3v2::v4;
        }

        original_properties = file.properties();
        original_unsupported = original_properties.unsupportedData();
        for (const auto& key : file.complexPropertyKeys()) {
            complex_properties.emplace_back(key, file.complexProperties(key));
        }
        if (cover) {
            auto picture_entry = std::find_if(complex_properties.begin(), complex_properties.end(),
                                              [](const auto& entry) { return entry.first == "PICTURE"; });
            if (picture_entry == complex_properties.end()) {
                complex_properties.emplace_back("PICTURE", PictureList());
                picture_entry = std::prev(complex_properties.end());
            }
            PictureList edited;
            for (const auto& picture : edited_pictures(to_vector(picture_entry->second), *cover)) edited.append(picture);
            picture_entry->second = edited;
        }
        for (const auto& [key, properties] : complex_properties) {
            if (!properties.isEmpty()) expected_complex_keys.insert(key.to8Bit(true));
        }

        TagLib::PropertyMap desired_properties = original_properties;
        for (const auto& [property, value] : changed_properties) {
            const TagLib::String tag_key(property, TagLib::String::UTF8);
            if (value.empty()) {
                desired_properties.erase(tag_key);
            } else {
                TagLib::StringList values;
                values.append(TagLib::String(value, TagLib::String::UTF8));
                desired_properties[tag_key] = values;
            }
        }

        const TagLib::PropertyMap rejected = file.setProperties(desired_properties);
        for (auto it = rejected.begin(); it != rejected.end(); ++it) {
            const auto changed = changed_properties.find(it->first.to8Bit(true));
            if (changed != changed_properties.end() && !changed->second.empty()) {
                throw std::runtime_error("Audio format cannot store requested tag '" +
                                         it->first.to8Bit(true) + "'");
            }
        }

        for (const auto& [key, properties] : complex_properties) {
            if (!file.setComplexProperties(key, properties)) {
                if (cover && key == "PICTURE")
                    throw std::runtime_error("This audio format cannot store a cover image: '" + path.string() + "'");
                throw std::runtime_error("Audio format could not preserve complex metadata in '" +
                                         path.string() + "'");
            }
        }

        verify_metadata_preserved(original_properties, expected_complex_keys, complex_properties,
                                  original_unsupported, file, changed_properties, cover != nullptr, path);
        const bool saved = mpeg
            ? mpeg->save(TagLib::MPEG::File::ID3v2 | (had_id3v1 ? TagLib::MPEG::File::ID3v1 : 0),
                         TagLib::File::StripNone, id3_version, TagLib::File::DoNotDuplicate)
            : file.save();
        if (!saved) {
            throw std::runtime_error("TagLib could not save temporary audio file: '" + path.string() + "'");
        }
    }

    TagLib::FileRef saved_file(path.c_str(), false);
    if (saved_file.isNull() || saved_file.file() == nullptr || !saved_file.file()->isValid()) {
        throw std::runtime_error("TagLib cannot verify saved temporary audio file: '" + path.string() + "'");
    }
    verify_metadata_preserved(original_properties, expected_complex_keys, complex_properties,
                              original_unsupported, saved_file, changed_properties, cover != nullptr, path);
}

void verify_changes(const Track& verified,
                    const std::vector<std::pair<std::string, std::string>>& changes) {
    for (const auto& [key, value] : changes) {
        const std::string normalized_key = display_tag_key(key);
        const auto found = verified.tags.find(normalized_key);
        if (value.empty()) {
            if (found != verified.tags.end()) {
                throw std::runtime_error("TagLib did not clear tag '" + normalized_key + "'");
            }
        } else if (found == verified.tags.end() || found->second != value) {
            throw std::runtime_error("TagLib did not store tag '" + normalized_key + "' as requested");
        }
    }
}

int rename_exchange(int directory_descriptor, const std::string& temporary_name,
                    const std::string& target_name) {
#if defined(SYS_renameat2)
    return static_cast<int>(::syscall(SYS_renameat2, directory_descriptor,
                                      temporary_name.c_str(), directory_descriptor,
                                      target_name.c_str(), RENAME_EXCHANGE));
#else
    errno = ENOSYS;
    return -1;
#endif
}

}

std::vector<std::filesystem::path> scan_audio(const std::filesystem::path& directory,
                                              bool recursive,
                                              std::size_t limit) {
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error("Cannot inspect audio directory", directory, error);
    }
    if (std::filesystem::is_symlink(root_status) || !std::filesystem::is_directory(root_status)) {
        throw std::invalid_argument("Audio scan root must be a directory, not a symlink: '" +
                                    directory.string() + "'");
    }
    if (limit == 0) {
        return {};
    }

    std::priority_queue<std::filesystem::path, std::vector<std::filesystem::path>, PathGreater> selected;
    const auto add_candidate = [&](const std::filesystem::directory_entry& entry) {
        std::error_code status_error;
        const auto status = entry.symlink_status(status_error);
        if (status_error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status) || !has_audio_extension(entry.path())) {
            return;
        }
        selected.push(entry.path());
        if (selected.size() > limit) {
            selected.pop();
        }
    };

    if (recursive) {
        std::filesystem::recursive_directory_iterator it(
            directory, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        while (it != end) {
            if (!error) {
                add_candidate(*it);
            }
            error.clear();
            it.increment(error);
        }
    } else {
        std::filesystem::directory_iterator it(
            directory, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::directory_iterator end;
        while (it != end) {
            if (!error) {
                add_candidate(*it);
            }
            error.clear();
            it.increment(error);
        }
    }

    std::vector<std::filesystem::path> result;
    result.reserve(selected.size());
    while (!selected.empty()) {
        result.push_back(selected.top());
        selected.pop();
    }
    std::sort(result.begin(), result.end(), PathGreater{});
    return result;
}

std::string detect_image_mime(const std::vector<std::uint8_t>& data) {
    static constexpr std::array<std::uint8_t, 3> jpeg{0xff, 0xd8, 0xff};
    static constexpr std::array<std::uint8_t, 8> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (data.size() >= jpeg.size() && std::equal(jpeg.begin(), jpeg.end(), data.begin())) return "image/jpeg";
    if (data.size() >= png.size() && std::equal(png.begin(), png.end(), data.begin())) return "image/png";
    return {};
}

std::uint64_t cover_fingerprint(const std::vector<std::uint8_t>& data) {
    if (data.empty()) return 0;
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : data) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

Cover load_cover_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::is_regular_file(status))
        throw std::runtime_error("Cover image is not a readable file: '" + path.string() + "'");
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw std::runtime_error("Cannot inspect cover image '" + path.string() + "': " + error.message());
    if (size > max_cover_bytes)
        throw std::runtime_error("Cover image is larger than 16 MB: '" + path.filename().string() + "'");
    Cover cover;
    cover.data.resize(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(cover.data.data()), static_cast<std::streamsize>(cover.data.size())))
        throw std::runtime_error("Cannot read cover image '" + path.string() + "'");
    cover.mime = detect_image_mime(cover.data);
    if (cover.mime.empty())
        throw std::runtime_error("Cover must be a JPEG or PNG image: '" + path.filename().string() + "'");
    return cover;
}

std::optional<Cover> read_cover(const std::filesystem::path& path) {
    TagLib::FileRef file(path.c_str(), false);
    if (file.isNull() || file.file() == nullptr || !file.file()->isValid())
        throw std::runtime_error("Unsupported or malformed audio file: '" + path.string() + "'");
    const auto pictures = to_vector(file.complexProperties("PICTURE"));
    const auto shown = shown_cover_index(pictures);
    if (!shown) return std::nullopt;
    Cover cover;
    cover.data = picture_bytes(pictures[*shown]);
    if (cover.data.empty()) return std::nullopt;
    cover.mime = detect_image_mime(cover.data);
    if (cover.mime.empty()) {
        const auto found = pictures[*shown].find("mimeType");
        if (found != pictures[*shown].end()) cover.mime = found->second.toString().to8Bit(true);
    }
    return cover;
}

Track read_track(const std::filesystem::path& path) {
    Track::SourceSnapshot snapshot;
    Track track = read_track_impl(path, snapshot);
    track.source_snapshot_ = snapshot;
    return track;
}

Track save_tags(const Track& original,
                const std::map<std::string, std::string>& changed,
                const CoverChange* cover) {
    if (changed.empty() && !cover) {
        return original;
    }
    if (!original.source_snapshot_.valid) {
        throw std::invalid_argument("Track was not loaded by read_track");
    }

    std::vector<std::pair<std::string, std::string>> changes;
    std::map<std::string, std::string> seen_keys;
    for (const auto& [key, value] : changed) {
        const std::string property = property_key_for_edit(key);
        const std::string normalized_key = display_tag_key(key);
        const auto [it, inserted] = seen_keys.emplace(property, normalized_key);
        if (!inserted) {
            throw std::invalid_argument("Tag key was supplied more than once: '" + it->second + "'");
        }
        const auto old_value = original.tags.find(normalized_key);
        if (value.empty()) {
            if (old_value != original.tags.end()) {
                changes.emplace_back(normalized_key, value);
            }
        } else if (old_value == original.tags.end() || old_value->second != value) {
            changes.emplace_back(normalized_key, value);
        }
    }
    std::uint64_t wanted_cover = 0;
    bool cover_needed = false;
    if (cover) {
        if (cover->image) {
            const std::string mime = detect_image_mime(cover->image->data);
            if (mime.empty() || mime != cover->image->mime || cover->image->data.size() > max_cover_bytes)
                throw std::invalid_argument("Cover must be a JPEG or PNG image of at most 16 MB");
            wanted_cover = cover_fingerprint(cover->image->data);
        }
        cover_needed = wanted_cover != original.cover_hash;
    }
    if (changes.empty() && !cover_needed) {
        return original;
    }
    if ((original.source_snapshot_.mode & 0222U) == 0) {
        throw std::system_error(EACCES, std::generic_category(),
                                "Audio file is read-only: '" + original.path.string() + "'");
    }

    const int source_descriptor = ::open(original.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source_descriptor < 0) {
        throw_errno("Cannot open", original.path);
    }
    FileDescriptor source(source_descriptor);
    const Track::SourceSnapshot opened_snapshot = stat_descriptor(source.get(), original.path);
    if (opened_snapshot.link_count != 1 || !same_snapshot(original.source_snapshot_, opened_snapshot)) {
        throw std::runtime_error("Audio file changed or has ambiguous links since it was read: '" +
                                 original.path.string() + "'");
    }

    std::filesystem::path directory = original.path.parent_path();
    if (directory.empty()) {
        directory = ".";
    }
    const std::string target_name = original.path.filename().string();
    if (target_name.empty() || target_name == "." || target_name == "..") {
        throw std::invalid_argument("Audio path must name a file: '" + original.path.string() + "'");
    }
    const int directory_descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (directory_descriptor < 0) {
        throw_errno("Cannot open audio directory", directory);
    }
    FileDescriptor target_directory(directory_descriptor);

    struct stat source_path_status {};
    if (::fstatat(target_directory.get(), target_name.c_str(), &source_path_status,
                  AT_SYMLINK_NOFOLLOW) < 0) {
        throw_errno("Cannot inspect", original.path);
    }
    if (!same_snapshot(original.source_snapshot_, snapshot_from_stat(source_path_status))) {
        throw std::runtime_error("Audio file changed since it was read: '" + original.path.string() + "'");
    }

    TemporaryFile temporary = create_temporary_file(directory, original.path);
    copy_source(source.get(), temporary.descriptor(), opened_snapshot.size, original.path);
    if (::fsync(temporary.descriptor()) < 0) {
        throw_errno("Cannot sync temporary audio file", temporary.path());
    }
    if (!same_snapshot(opened_snapshot, stat_descriptor(source.get(), original.path))) {
        throw std::runtime_error("Audio file changed while it was being copied: '" + original.path.string() + "'");
    }

    update_temporary_tags(temporary.path(), changes, cover_needed ? cover : nullptr);
    Track::SourceSnapshot verified_snapshot;
    Track verified_temporary = read_track_impl(temporary.path(), verified_snapshot);
    verified_temporary.source_snapshot_ = verified_snapshot;
    verify_changes(verified_temporary, changes);
    if (cover_needed && verified_temporary.cover_hash != wanted_cover)
        throw std::runtime_error("TagLib did not store the cover as requested");

    const int edited_descriptor =
        ::open(temporary.path().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (edited_descriptor < 0) {
        throw_errno("Cannot reopen temporary audio file", temporary.path());
    }
    FileDescriptor edited_file(edited_descriptor);
    const Track::SourceSnapshot edited_snapshot = stat_descriptor(edited_file.get(), temporary.path());
    if (edited_snapshot.link_count != 1) {
        throw std::runtime_error("Temporary audio file has ambiguous links: '" +
                                 temporary.path().string() + "'");
    }

    if ((edited_snapshot.owner != original.source_snapshot_.owner ||
         edited_snapshot.group != original.source_snapshot_.group) &&
        ::fchown(edited_file.get(), static_cast<uid_t>(original.source_snapshot_.owner),
                 static_cast<gid_t>(original.source_snapshot_.group)) < 0) {
        throw_errno("Cannot preserve audio file ownership", temporary.path());
    }
    copy_xattrs(source.get(), edited_file.get(), original.path);
    if (::fchmod(edited_file.get(), static_cast<mode_t>(original.source_snapshot_.mode)) < 0) {
        throw_errno("Cannot preserve audio file permissions", temporary.path());
    }
    if (::fsync(edited_file.get()) < 0) {
        throw_errno("Cannot sync temporary audio file", temporary.path());
    }

    const Track::SourceSnapshot descriptor_before_commit = stat_descriptor(source.get(), original.path);
    const Track::SourceSnapshot path_before_commit =
        stat_path_at(target_directory.get(), target_name, original.path);
    if (!same_snapshot(original.source_snapshot_, descriptor_before_commit) ||
        !same_snapshot(original.source_snapshot_, path_before_commit)) {
        throw std::runtime_error("Audio file changed before tags could be saved: '" +
                                 original.path.string() + "'");
    }

    verified_temporary.path = original.path;
    verified_temporary.format = original.format;
    const std::string temporary_name = temporary.path().filename().string();
    if (rename_exchange(target_directory.get(), temporary_name, target_name) < 0) {
        throw_errno("Cannot atomically replace audio file", original.path);
    }

    bool exchanged = true;
    temporary.preserve_path();
    const auto restore_original = [&]() {
        if (!exchanged) return true;
        if (rename_exchange(target_directory.get(), temporary_name, target_name) == 0) {
            exchanged = false;
            temporary.allow_cleanup();
            ::fsync(target_directory.get());
            return true;
        }
        return false;
    };

    Track::SourceSnapshot committed_snapshot;
    try {
        const Track::SourceSnapshot old_target =
            stat_path_at(target_directory.get(), temporary_name, original.path);
        if (!same_moved_source(original.source_snapshot_, old_target)) {
            throw std::runtime_error("Audio file changed during save: '" + original.path.string() + "'");
        }
        committed_snapshot = stat_descriptor(edited_file.get(), original.path);
        if (::fsync(target_directory.get()) < 0) {
            throw_errno("Cannot sync audio directory", directory);
        }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        if (!restore_original()) {
            throw std::runtime_error("Save failed and rollback could not restore the original; "
                                     "the original file is preserved at '" +
                                     temporary.path().string() + "'");
        }
        std::rethrow_exception(failure);
    }

    temporary.allow_cleanup();
    verified_temporary.source_snapshot_ = committed_snapshot;

    if (::unlinkat(target_directory.get(), temporary_name.c_str(), 0) == 0) {
        temporary.preserve_path();
        ::fsync(target_directory.get());
    } else {
        temporary.allow_cleanup();
    }
    exchanged = false;
    return verified_temporary;
}

}
