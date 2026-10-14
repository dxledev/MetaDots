#include "../src/core/metadata.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/tvariant.h>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void require_throws(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "metadots-metadata-tests-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* created = ::mkdtemp(buffer.data());
        if (created == nullptr) {
            throw std::runtime_error("Could not create metadata test directory");
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
}

void append_chunk(std::vector<std::uint8_t>& bytes, const char* name,
                  const std::vector<std::uint8_t>& contents) {
    bytes.insert(bytes.end(), name, name + 4);
    append_u32(bytes, static_cast<std::uint32_t>(contents.size()));
    bytes.insert(bytes.end(), contents.begin(), contents.end());
    if ((contents.size() & 1U) != 0) {
        bytes.push_back(0);
    }
}

void write_wav(const std::filesystem::path& path) {
    constexpr std::uint32_t sample_rate = 8000;
    constexpr std::uint16_t channel_count = 1;
    constexpr std::uint16_t bits_per_sample = 16;
    constexpr std::uint32_t sample_count = 800;
    std::vector<std::uint8_t> format;
    append_u16(format, 1);
    append_u16(format, channel_count);
    append_u32(format, sample_rate);
    append_u32(format, sample_rate * channel_count * bits_per_sample / 8);
    append_u16(format, channel_count * bits_per_sample / 8);
    append_u16(format, bits_per_sample);

    std::vector<std::uint8_t> audio(sample_count * channel_count * bits_per_sample / 8, 0);
    std::vector<std::uint8_t> riff;
    riff.insert(riff.end(), {'W', 'A', 'V', 'E'});
    append_chunk(riff, "fmt ", format);
    append_chunk(riff, "data", audio);

    std::vector<std::uint8_t> file;
    file.insert(file.end(), {'R', 'I', 'F', 'F'});
    append_u32(file, static_cast<std::uint32_t>(riff.size()));
    file.insert(file.end(), riff.begin(), riff.end());

    std::ofstream output(path, std::ios::binary);
    if (!output || !output.write(reinterpret_cast<const char*>(file.data()),
                                 static_cast<std::streamsize>(file.size()))) {
        throw std::runtime_error("Could not write WAV fixture");
    }
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> wav_audio_data(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    require(bytes.size() >= 12 && std::string(bytes.begin(), bytes.begin() + 4) == "RIFF" &&
                std::string(bytes.begin() + 8, bytes.begin() + 12) == "WAVE",
            "WAV container became invalid");

    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const std::string chunk_name(bytes.begin() + offset, bytes.begin() + offset + 4);
        const std::uint32_t chunk_size = static_cast<std::uint32_t>(bytes[offset + 4]) |
                                         (static_cast<std::uint32_t>(bytes[offset + 5]) << 8) |
                                         (static_cast<std::uint32_t>(bytes[offset + 6]) << 16) |
                                         (static_cast<std::uint32_t>(bytes[offset + 7]) << 24);
        offset += 8;
        require(chunk_size <= bytes.size() - offset, "WAV chunk exceeds file size");
        if (chunk_name == "data") {
            return {bytes.begin() + offset, bytes.begin() + offset + chunk_size};
        }
        offset += chunk_size + (chunk_size & 1U);
    }
    throw std::runtime_error("WAV audio data chunk was lost");
}

void add_multivalue_artist_fixture(const std::filesystem::path& path) {
    TagLib::FileRef file(path.c_str(), false);
    require(!file.isNull(), "TagLib could not open multivalue fixture");
    TagLib::StringList artists;
    artists.append(TagLib::String("Artist One", TagLib::String::UTF8));
    artists.append(TagLib::String("Artist Two", TagLib::String::UTF8));
    TagLib::PropertyMap properties = file.properties();
    properties["ARTIST"] = artists;
    require(file.setProperties(properties).isEmpty(),
            "TagLib could not create multivalue artist fixture");
    require(file.save(), "TagLib could not save multivalue artist fixture");
}

TagLib::List<TagLib::VariantMap> add_artwork_fixture(const std::filesystem::path& path) {
    TagLib::FileRef file(path.c_str(), false);
    require(!file.isNull(), "TagLib could not open artwork fixture");
    TagLib::VariantMap picture;
    picture["data"] = TagLib::ByteVector("cover-image", 11);
    picture["description"] = TagLib::String("test cover");
    picture["mimeType"] = TagLib::String("image/png");
    picture["pictureType"] = TagLib::String("Front Cover");
    TagLib::List<TagLib::VariantMap> pictures;
    pictures.append(picture);
    require(file.setComplexProperties("PICTURE", pictures),
            "TagLib could not create artwork fixture");
    require(file.save(), "TagLib could not save artwork fixture");
    return file.complexProperties("PICTURE");
}

void test_read_scan_and_round_trip(const std::filesystem::path& root) {
    const auto audio = root / "café track.wav";
    const auto later = root / "zeta.wav";
    const auto nested = root / "sub" / "alpha.wav";
    std::filesystem::create_directories(nested.parent_path());
    write_wav(audio);
    write_wav(later);
    write_wav(nested);
    require(::chmod(audio.c_str(), 0640) == 0, "Could not set fixture permissions");
    add_multivalue_artist_fixture(audio);
    const auto original_audio_data = wav_audio_data(audio);
    const auto original_artwork = add_artwork_fixture(audio);
    constexpr char attribute_name[] = "user.metadots-test";
    constexpr char attribute_value[] = "preserve me";
    bool xattrs_supported = false;
    if (::setxattr(audio.c_str(), attribute_name, attribute_value,
                   sizeof(attribute_value) - 1, 0) == 0) {
        xattrs_supported = true;
    } else if (errno != ENOTSUP && errno != EOPNOTSUPP) {
        throw std::runtime_error("Could not set WAV extended attribute");
    }
    if (xattrs_supported && ::getxattr(audio.c_str(), attribute_name, nullptr, 0) < 0) {
        throw std::runtime_error("TagLib fixture setup lost its extended attribute");
    }
    struct stat original_status {};
    require(::stat(audio.c_str(), &original_status) == 0, "Could not inspect WAV fixture");

    const auto basic_scan = metadots::scan_audio(root, false);
    require(basic_scan.size() == 2, "Nonrecursive scan included a nested file");
    require(std::is_sorted(basic_scan.begin(), basic_scan.end()), "Scan results are not sorted");
    require(metadots::scan_audio(root, false, 1) ==
                std::vector<std::filesystem::path>{basic_scan.front()},
            "Scan limit did not select the first sorted result");
    const auto recursive_scan = metadots::scan_audio(root, true, 2);
    require(recursive_scan.size() == 2 && std::is_sorted(recursive_scan.begin(), recursive_scan.end()),
            "Recursive bounded scan is not sorted or did not enforce its limit");
    require(metadots::scan_audio(root, true, 0).empty(), "Zero scan limit returned audio files");

    const auto symlink = root / "linked.wav";
    std::filesystem::create_symlink(audio, symlink);
    std::filesystem::create_directory_symlink(nested.parent_path(), root / "linked-directory");
    require(metadots::scan_audio(root, false).size() == 2,
            "Audio scan followed or included a symlink");
    require(metadots::scan_audio(root, true).size() == 3,
            "Recursive audio scan followed a symlinked directory");
    require_throws([&] { (void)metadots::read_track(symlink); },
                   "read_track accepted a symlink");

    const metadots::Track initial = metadots::read_track(audio);
    require(initial.path == audio && initial.format == "WAV", "Track path or format is incorrect");
    require(initial.sample_rate == 8000, "WAV sample rate was not read");
    require(initial.tags.at("artist") == "Artist One; Artist Two",
            "Multi-value artist field was not read correctly");

    const metadots::Track first_save = metadots::save_tags(initial, {{"title", "夜の音"}});
    require(first_save.tags.contains("title") && first_save.tags.at("title") == "夜の音",
            "UTF-8 title did not round-trip");
    require(first_save.tags.contains("artist") &&
                first_save.tags.at("artist") == "Artist One; Artist Two",
            "Changing title did not preserve a multivalue artist field");
    require(wav_audio_data(audio) == original_audio_data,
            "Saving tags changed the WAV audio payload");
    require((std::filesystem::status(audio).permissions() & std::filesystem::perms::all) ==
                (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                 std::filesystem::perms::group_read),
            "Saving tags did not preserve file permissions");
    struct stat saved_status {};
    require(::stat(audio.c_str(), &saved_status) == 0 &&
                saved_status.st_uid == original_status.st_uid &&
                saved_status.st_gid == original_status.st_gid,
            "Saving tags did not preserve file ownership");
    require(first_save.format == "WAV", "Saved track lost its format label");
    {
        TagLib::FileRef saved_file(audio.c_str(), false);
        require(saved_file.complexProperties("PICTURE") == original_artwork,
                "Saving tags changed embedded artwork");
    }
    if (xattrs_supported) {
        std::array<char, sizeof(attribute_value)> saved_attribute{};
        const ssize_t size = ::getxattr(audio.c_str(), attribute_name,
                                        saved_attribute.data(), saved_attribute.size());
        require(size == static_cast<ssize_t>(sizeof(attribute_value) - 1) &&
                    std::string(saved_attribute.data(), static_cast<std::size_t>(size)) == attribute_value,
                "Saving tags did not preserve extended attributes");
    }

    const metadots::Track second_save = metadots::save_tags(
        first_save, {{"genre", "Ambient"}, {"copyright", "© 2026"}});
    require(second_save.tags.contains("title") && second_save.tags.at("title") == "夜の音" &&
                second_save.tags.contains("artist") &&
                second_save.tags.at("artist") == "Artist One; Artist Two" &&
                second_save.tags.contains("genre") && second_save.tags.at("genre") == "Ambient" &&
                second_save.tags.contains("copyright") &&
                second_save.tags.at("copyright") == "© 2026",
            "Changing one field did not retain other tags");

    const metadots::Track changed_artist = metadots::save_tags(second_save, {{"artist", "Zoë"}});
    require(changed_artist.tags.at("artist") == "Zoë", "UTF-8 artist did not round-trip");
    const metadots::Track cleared = metadots::save_tags(changed_artist, {{"artist", ""}});
    require(cleared.tags.count("artist") == 0 && cleared.tags.contains("title") &&
                cleared.tags.at("title") == "夜の音" && cleared.tags.contains("genre") &&
                cleared.tags.at("genre") == "Ambient" && cleared.tags.at("copyright") == "© 2026",
            "Clearing one field changed other tags");
}

void test_rejects_bad_inputs_and_conflicts(const std::filesystem::path& root) {
    const auto additional_formats = root / "additional-formats";
    std::filesystem::create_directory(additional_formats);
    for (const std::string extension : {"mp4", "wma", "asf"}) {
        std::ofstream output(additional_formats / ("candidate." + extension));
        output << "scan candidate";
    }
    const auto format_candidates = metadots::scan_audio(additional_formats);
    require(format_candidates.size() == 3,
            "Scanner did not include MP4 and ASF audio candidates");

    const auto malformed = root / "malformed.mp3";
    {
        std::ofstream output(malformed, std::ios::binary);
        output << "this is not an audio file";
    }
    require_throws([&] { (void)metadots::read_track(malformed); },
                   "read_track accepted malformed audio");

    const auto unsupported = root / "notes.txt";
    {
        std::ofstream output(unsupported);
        output << "not audio";
    }
    require_throws([&] { (void)metadots::read_track(unsupported); },
                   "read_track accepted an unsupported format");

    const auto readonly = root / "readonly.wav";
    write_wav(readonly);
    require(::chmod(readonly.c_str(), 0444) == 0, "Could not make read-only fixture");
    const metadots::Track readonly_track = metadots::read_track(readonly);
    const auto readonly_before = read_bytes(readonly);
    require_throws([&] { (void)metadots::save_tags(readonly_track, {{"title", "blocked"}}); },
                   "save_tags replaced a read-only source");
    require(read_bytes(readonly) == readonly_before &&
                (std::filesystem::status(readonly).permissions() & std::filesystem::perms::all) ==
                    (std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
                     std::filesystem::perms::others_read),
            "Read-only rejection changed the source");

    const auto audio = root / "conflict.wav";
    write_wav(audio);
    const metadots::Track stale = metadots::read_track(audio);
    const auto before_unknown_key = read_bytes(audio);
    require_throws([&] { (void)metadots::save_tags(stale, {{"mood", "calm"}}); },
                   "save_tags accepted an unknown editable key");
    require(read_bytes(audio) == before_unknown_key, "Unknown tag key changed the source file");

    {
        std::ofstream output(audio, std::ios::binary | std::ios::app);
        output.put('x');
    }
    const auto externally_changed = read_bytes(audio);
    require_throws([&] { (void)metadots::save_tags(stale, {{"title", "new value"}}); },
                   "save_tags overwrote a source changed after read_track");
    require(read_bytes(audio) == externally_changed,
            "Conflict rejection changed the externally modified source");

    const auto linked_audio = root / "hardlink-source.wav";
    const auto hard_link = root / "hardlink-copy.wav";
    write_wav(linked_audio);
    std::filesystem::create_hard_link(linked_audio, hard_link);
    const metadots::Track ambiguous = metadots::read_track(linked_audio);
    const auto linked_before = read_bytes(linked_audio);
    require_throws([&] { (void)metadots::save_tags(ambiguous, {{"title", "new value"}}); },
                   "save_tags accepted a multiply linked source");
    require(read_bytes(linked_audio) == linked_before && read_bytes(hard_link) == linked_before,
            "Hard-link rejection changed a source file");
}


// Writes a few silent MPEG frames with a hand-built ID3v2 header and no ID3v1 tag.
void write_mp3(const std::filesystem::path& path, std::uint8_t id3_major, const std::string& track) {
    auto syncsafe = [](std::uint32_t value) {
        return std::array<char, 4>{static_cast<char>((value >> 21) & 0x7f), static_cast<char>((value >> 14) & 0x7f),
                                   static_cast<char>((value >> 7) & 0x7f), static_cast<char>(value & 0x7f)};
    };
    const std::string text = std::string(1, '\0') + track;
    std::string frame = "TRCK";
    const auto size = syncsafe(static_cast<std::uint32_t>(text.size()));
    if (id3_major == 4) frame.append(size.data(), 4);
    else for (int shift = 24; shift >= 0; shift -= 8) frame.push_back(static_cast<char>((text.size() >> shift) & 0xff));
    frame += std::string(2, '\0') + text;
    std::string data = "ID3";
    data.push_back(static_cast<char>(id3_major));
    data.push_back('\0');
    data.push_back('\0');
    const auto tag_size = syncsafe(static_cast<std::uint32_t>(frame.size()));
    data.append(tag_size.data(), 4);
    data += frame;
    for (int index = 0; index < 4; ++index) {
        data += std::string("\xff\xfb\x90\x00", 4);
        data += std::string(413, '\0');
    }
    std::ofstream(path, std::ios::binary) << data;
}

void test_mp3_keeps_id3_layout(const std::filesystem::path& root) {
    for (const std::uint8_t major : {std::uint8_t{3}, std::uint8_t{4}}) {
        const auto audio = root / ("id3v2." + std::to_string(major) + ".mp3");
        write_mp3(audio, major, "7");
        const auto saved = metadots::save_tags(metadots::read_track(audio), {{"album", "Same album"}});
        const auto bytes = read_bytes(audio);
        require(bytes.size() > 10 && bytes[3] == static_cast<char>(major), "Saving an MP3 changed its ID3v2 version");
        require(std::string(bytes.end() - 128, bytes.end() - 125) != "TAG", "Saving an MP3 added an ID3v1 tag");
        require(saved.tags.at("track") == "7" && saved.tags.at("album") == "Same album", "MP3 track number or album was not kept");
    }
}

// Bytes that start like a PNG or JPEG; the core only checks the signature, never decodes.
metadots::Cover fake_cover(const std::string& mime, std::uint8_t seed) {
    metadots::Cover cover;
    cover.mime = mime;
    cover.data = mime == "image/png" ? std::vector<std::uint8_t>{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}
                                     : std::vector<std::uint8_t>{0xff, 0xd8, 0xff, 0xe0};
    for (int index = 0; index < 200; ++index) cover.data.push_back(static_cast<std::uint8_t>(seed + index));
    return cover;
}

metadots::CoverChange cover_change(const metadots::Cover& cover) {
    return {std::make_shared<const metadots::Cover>(cover)};
}

TagLib::VariantMap make_picture(const metadots::Cover& cover, const char* type) {
    TagLib::VariantMap picture;
    picture["data"] = TagLib::ByteVector(reinterpret_cast<const char*>(cover.data.data()),
                                         static_cast<unsigned int>(cover.data.size()));
    picture["mimeType"] = TagLib::String(cover.mime);
    picture["pictureType"] = TagLib::String(type);
    picture["description"] = TagLib::String();
    return picture;
}

std::vector<std::vector<std::uint8_t>> picture_payloads(const std::filesystem::path& path) {
    TagLib::FileRef file(path.c_str(), false);
    std::vector<std::vector<std::uint8_t>> result;
    for (const auto& picture : file.complexProperties("PICTURE")) {
        const auto data = picture.value("data").toByteVector();
        result.emplace_back(data.begin(), data.end());
    }
    return result;
}

// Adds, replaces and removes a cover on one file, checking that tags and other pictures survive.
void check_cover_round_trip(const std::filesystem::path& audio, const std::string& label);
void check_cover_round_trip_unlabeled(const std::filesystem::path& audio, const std::string& label) {
    const auto first = fake_cover("image/png", 10);
    const auto second = fake_cover("image/jpeg", 90);
    auto track = metadots::read_track(audio);
    require(track.cover_hash == 0 && !metadots::read_cover(audio), label + ": fixture already has a cover");

    const auto add = cover_change(first);
    track = metadots::save_tags(track, {{"album", "Cover album"}}, &add);
    require(track.cover_hash == metadots::cover_fingerprint(first.data) && track.cover_size == first.data.size(), label + ": added cover not reported");
    require(track.tags.at("album") == "Cover album", label + ": tag edit lost beside a cover edit");
    const auto stored = metadots::read_cover(audio);
    require(stored && stored->data == first.data && stored->mime == "image/png", label + ": stored cover differs");

    const auto replace = cover_change(second);
    track = metadots::save_tags(track, {}, &replace);
    const auto replaced = metadots::read_cover(audio);
    require(replaced && replaced->data == second.data && replaced->mime == "image/jpeg", label + ": replaced cover differs");
    require(track.tags.at("album") == "Cover album", label + ": cover-only save changed tags");
    require(picture_payloads(audio).size() == 1, label + ": replacing left the old cover behind");

    const auto size_before = read_bytes(audio);
    const auto same = metadots::save_tags(track, {}, &replace);
    require(read_bytes(audio) == size_before && same.cover_hash == track.cover_hash, label + ": saving an unchanged cover rewrote the file");

    const metadots::CoverChange remove{};
    track = metadots::save_tags(track, {}, &remove);
    require(track.cover_hash == 0 && !metadots::read_cover(audio) && picture_payloads(audio).empty(), label + ": cover was not removed");
    require(track.tags.at("album") == "Cover album", label + ": removing the cover changed tags");
    const auto removed_again = read_bytes(audio);
    (void)metadots::save_tags(track, {}, &remove);
    require(read_bytes(audio) == removed_again, label + ": removing a missing cover rewrote the file");
}

void check_cover_round_trip(const std::filesystem::path& audio, const std::string& label) {
    try { check_cover_round_trip_unlabeled(audio, label); }
    catch (const std::exception& error) { throw std::runtime_error(label + ": " + error.what()); }
}

void test_cover_edits(const std::filesystem::path& root) {
    for (const std::uint8_t major : {std::uint8_t{3}, std::uint8_t{4}}) {
        const auto audio = root / ("cover-id3v2." + std::to_string(major) + ".mp3");
        write_mp3(audio, major, "5");
        check_cover_round_trip(audio, "mp3 v2." + std::to_string(major));
        const auto bytes = read_bytes(audio);
        require(bytes[3] == static_cast<char>(major), "Cover edit changed the ID3v2 version");
        require(std::string(bytes.end() - 128, bytes.end() - 125) != "TAG", "Cover edit added an ID3v1 tag");
        require(metadots::read_track(audio).tags.at("track") == "5", "Cover edit lost the track number");
    }
    const auto wav = root / "cover.wav";
    write_wav(wav);
    check_cover_round_trip(wav, "wav");

    // Only the cover is touched: a back cover and its position among the pictures survive replace and remove.
    const auto layered = root / "layered.mp3";
    write_mp3(layered, 4, "1");
    const auto original_front = fake_cover("image/png", 1);
    const auto back = fake_cover("image/jpeg", 2);
    {
        TagLib::FileRef file(layered.c_str(), false);
        TagLib::List<TagLib::VariantMap> pictures;
        pictures.append(make_picture(back, "Back Cover"));
        pictures.append(make_picture(original_front, "Front Cover"));
        require(file.setComplexProperties("PICTURE", pictures) && file.save(), "Could not build layered picture fixture");
    }
    auto track = metadots::read_track(layered);
    require(track.cover_hash == metadots::cover_fingerprint(original_front.data), "A back cover was treated as the cover");
    const auto replacement = fake_cover("image/png", 50);
    const auto replace = cover_change(replacement);
    track = metadots::save_tags(track, {}, &replace);
    auto payloads = picture_payloads(layered);
    require(payloads.size() == 2 && payloads[0] == back.data && payloads[1] == replacement.data, "Replacing the cover disturbed the back cover");
    const metadots::CoverChange remove{};
    track = metadots::save_tags(track, {}, &remove);
    payloads = picture_payloads(layered);
    require(payloads.size() == 1 && payloads[0] == back.data && track.cover_hash == 0, "Removing the cover disturbed the back cover");

    // A cover saved as "Other" counts as the cover, so remove never leaves the shown image in place.
    const auto other = root / "other.mp3";
    write_mp3(other, 3, "2");
    {
        TagLib::FileRef file(other.c_str(), false);
        TagLib::List<TagLib::VariantMap> pictures;
        pictures.append(make_picture(original_front, "Other"));
        require(file.setComplexProperties("PICTURE", pictures) && file.save(), "Could not build Other picture fixture");
    }
    track = metadots::read_track(other);
    require(track.cover_hash == metadots::cover_fingerprint(original_front.data), "An Other picture was not shown as the cover");
    track = metadots::save_tags(track, {}, &remove);
    require(track.cover_hash == 0 && picture_payloads(other).empty(), "Remove left the shown cover in place");

    const auto bad = root / "bad-cover.mp3";
    write_mp3(bad, 4, "3");
    const auto before = read_bytes(bad);
    metadots::Cover not_an_image;
    not_an_image.mime = "image/png";
    not_an_image.data = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    const auto invalid = cover_change(not_an_image);
    require_throws([&] { (void)metadots::save_tags(metadots::read_track(bad), {}, &invalid); }, "A non-image was accepted as a cover");
    auto mislabeled = fake_cover("image/jpeg", 3);
    mislabeled.mime = "image/png";
    const auto mislabeled_change = cover_change(mislabeled);
    require_throws([&] { (void)metadots::save_tags(metadots::read_track(bad), {}, &mislabeled_change); }, "A mislabeled image was accepted");
    require(read_bytes(bad) == before, "Rejected cover edits changed the file");

    const auto text = root / "notes.txt";
    std::ofstream(text) << "not an image";
    require_throws([&] { (void)metadots::load_cover_file(text); }, "load_cover_file accepted a text file");
    require_throws([&] { (void)metadots::load_cover_file(root / "missing.png"); }, "load_cover_file accepted a missing file");
    const auto png = root / "cover.png";
    {
        const auto cover = fake_cover("image/png", 7);
        std::ofstream(png, std::ios::binary).write(reinterpret_cast<const char*>(cover.data.data()), static_cast<std::streamsize>(cover.data.size()));
    }
    const auto loaded = metadots::load_cover_file(png);
    require(loaded.mime == "image/png" && loaded.data == fake_cover("image/png", 7).data, "load_cover_file returned the wrong bytes");
    const auto huge = root / "huge.png";
    {
        std::ofstream out(huge, std::ios::binary);
        out.write("\x89PNG\r\n\x1a\n", 8);
        out.seekp(static_cast<std::streamoff>(metadots::max_cover_bytes));
        out.put('x');
    }
    require_throws([&] { (void)metadots::load_cover_file(huge); }, "load_cover_file accepted an oversized image");

    // FLAC and M4A fixtures need an encoder, so those checks only run where ffmpeg is installed.
    if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
        std::cout << "skipped FLAC and M4A cover checks: ffmpeg not found\n";
        return;
    }
    for (const char* extension : {"flac", "m4a", "ogg", "opus"}) {
        const auto encoded = root / (std::string("encoded.") + extension);
        const std::string command = "ffmpeg -loglevel error -y -f lavfi -i anullsrc=r=44100:cl=mono -t 1 '" + encoded.string() + "' >/dev/null 2>&1";
        if (std::system(command.c_str()) != 0) {
            std::cout << "skipped " << extension << " cover checks: ffmpeg could not encode it\n";
            continue;
        }
        check_cover_round_trip(encoded, extension);
    }
}
}

int main() {
    try {
        TemporaryDirectory temporary_directory;
        test_read_scan_and_round_trip(temporary_directory.path());
        test_rejects_bad_inputs_and_conflicts(temporary_directory.path());
        test_mp3_keeps_id3_layout(temporary_directory.path());
        test_cover_edits(temporary_directory.path());
        std::cout << "metadata tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "metadata tests failed: " << error.what() << '\n';
        return 1;
    }
}
