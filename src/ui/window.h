#pragma once
#include "core/metadata.h"
#include "core/theme.h"
#include <gtk/gtk.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace metadots {
struct Document {
    Track track;
    std::map<std::string, std::string> pending;
    unsigned carried{};
    // A staged cover change: an image replaces the cover, a null image removes it.
    std::optional<CoverChange> cover_edit;
    std::uint64_t cover_edit_hash{};
    bool dirty() const { return !pending.empty() || cover_edit.has_value(); }
    // Fingerprint of the cover this file will have once saved; 0 means none.
    std::uint64_t cover_hash() const { return cover_edit ? cover_edit_hash : track.cover_hash; }
};

class Window {
public:
    Window(GtkApplication* application, std::filesystem::path config,
           std::vector<std::filesystem::path> paths);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    void present();
    bool self_test(const std::filesystem::path& screenshots, bool capture_modal = false);
private:
    GtkWidget *window_{}, *overlay_{}, *content_{}, *blur_{}, *modal_{}, *modal_card_{}, *modal_viewport_{}, *modal_feedback_{};
    GtkWidget *files_{}, *search_{}, *editor_{}, *selection_label_{}, *details_{}, *status_{}, *save_{};
    GtkCssProvider* css_{};
    GdkDisplay* display_{};
    guint timer_{};
    guint theme_debounce_{};
    GFileMonitor* theme_monitor_{};
    GFile* theme_file_{};
    gulong theme_monitor_handler_{};
    Theme theme_;
    std::filesystem::path config_;
    std::vector<Document> documents_;
    std::map<std::string, GtkWidget*> fields_;
    std::map<std::string, std::string> carry_;
    unsigned carry_generation_{};
    GtkWidget* carry_toggle_{};
    GtkWidget* explicit_toggle_{};
    std::optional<CoverChange> carry_cover_;
    std::uint64_t carry_cover_hash_{};
    GtkWidget *cover_frame_{}, *cover_stack_{}, *cover_picture_{}, *cover_note_{}, *cover_info_{}, *cover_remove_{};
    GdkTexture* cover_texture_{};
    std::uint64_t cover_texture_hash_{};
    std::string cover_texture_mime_;
    std::size_t cover_texture_bytes_{};
    bool browser_images_{};
    bool updating_{}, closing_{}, destroyed_{}, close_after_save_{};
    std::string last_theme_error_;
    std::filesystem::path browser_path_;
    GtkWidget *browser_entry_{}, *browser_list_{};
    std::function<void()> modal_action_;
    void build();
    void setup_keyboard();
    void setup_drop();
    void watch_theme();
    void queue_theme_reload();
    void close_window() noexcept;
    void cleanup() noexcept;
    GtkWidget* build_header();
    GtkWidget* build_library();
    GtkWidget* build_editor();
    GtkWidget* button(const char* text, const char* icon, std::function<void()> action);
    void connect_action(GtkWidget* widget, const char* signal, std::function<void()> action);
    void guarded(const std::function<void()>& action) noexcept;
    void show_error(const char* message) noexcept;
    void set_status(const std::string& message);
    void apply_theme(const Theme& theme);
    void refresh_theme();
    void open_paths(const std::vector<std::filesystem::path>& paths, bool recursive);
    void rebuild_files();
    void filter_files();
    std::vector<std::size_t> selected() const;
    void refresh_editor();
    void edit_field(const std::string& key);
    void stage_tag(Document& document, const std::string& key, const std::string& text);
    void refresh_explicit();
    void toggle_explicit();
    void clear_field(const std::string& key);
    std::string apply_carry();
    GtkWidget* build_cover();
    void refresh_cover();
    void choose_cover();
    void remove_cover();
    void set_cover_from_file(const std::filesystem::path& path);
    void stage_cover(const CoverChange& change);
    void refresh_dirty();
    void refresh_dirty_indicators();
    std::size_t dirty_count() const;
    void request_save();
    void save_changes();
    void request_revert();
    void request_close();
    void show_modal(const std::string& title, const std::string& message,
                    const std::string& confirm, std::function<void()> action);
    GtkWidget* begin_modal(const std::string& title);
    void dismiss_modal();
    void open_browser(bool images = false);
    void browse(const std::filesystem::path& path);
    void browser_accept();
    void screenshot(const std::filesystem::path& path);
};
int run_window(const std::filesystem::path& config, const std::vector<std::filesystem::path>& paths,
               bool self_test = false, const std::filesystem::path& screenshots = {});
}
