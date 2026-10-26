#include "window.h"
#include "blur_bin.h"
#include "core/gtk_config.h"
#include "core/settings.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cmath>
#include <unistd.h>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace metadots {
namespace {
constexpr std::array<std::pair<const char*, const char*>, 11> tag_fields{{
    {"title", "Title"}, {"artist", "Artist"}, {"album", "Album"},
    {"album_artist", "Album artist"}, {"track", "Track"}, {"disc", "Disc"},
    {"date", "Date"}, {"genre", "Genre"}, {"composer", "Composer"},
    {"comment", "Comment"}, {"copyright", "Copyright"}
}};

GtkWidget* label(const std::string& text, const char* style = nullptr) {
    auto* widget = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (style) gtk_widget_add_css_class(widget, style);
    return widget;
}

GtkWidget* scroll(GtkWidget* child) {
    auto* widget = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(widget), child);
    gtk_widget_set_vexpand(widget, true);
    return widget;
}

void margins(GtkWidget* widget, int value) {
    gtk_widget_set_margin_start(widget, value);
    gtk_widget_set_margin_end(widget, value);
    gtk_widget_set_margin_top(widget, value);
    gtk_widget_set_margin_bottom(widget, value);
}

std::string value(const Document& document, const std::string& key) {
    if (const auto found = document.pending.find(key); found != document.pending.end()) return found->second;
    if (const auto found = document.track.tags.find(key); found != document.track.tags.end()) return found->second;
    return {};
}

// Apple Music marks explicit songs with a boxed "E". Tags are plain text, so the title ends with
// U+1F174 (negative squared E), which draws that box in the text colour with the E cut out of it.
constexpr std::string_view explicit_suffix = " \xF0\x9F\x85\xB4";

bool has_explicit(const std::string& title) {
    return title.size() > explicit_suffix.size() && title.compare(title.size() - explicit_suffix.size(), explicit_suffix.size(), explicit_suffix) == 0;
}

std::string with_explicit(std::string title) {
    while (!title.empty() && g_ascii_isspace(static_cast<guchar>(title.back()))) title.pop_back();
    if (title.empty() || has_explicit(title)) return title;
    return title + std::string(explicit_suffix);
}

std::string without_explicit(std::string title) {
    if (has_explicit(title)) title.resize(title.size() - explicit_suffix.size());
    return title;
}

std::string folded(const std::string& text) {
    auto* utf8 = g_utf8_casefold(text.c_str(), -1);
    std::string result = utf8 ? utf8 : "";
    g_free(utf8);
    return result;
}

std::string count_of(std::size_t amount, const char* noun) {
    return std::to_string(amount) + " " + noun + (amount == 1 ? "" : "s");
}

bool is_image_path(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png";
}

std::vector<std::filesystem::path> scan_images(const std::filesystem::path& directory, std::size_t limit) {
    std::vector<std::filesystem::path> images;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && !entry_error && is_image_path(it->path())) images.push_back(it->path());
    }
    std::sort(images.begin(), images.end());
    if (images.size() > limit) images.resize(limit);
    return images;
}

std::string size_text(std::size_t bytes) {
    char text[32];
    if (bytes >= 1024 * 1024) g_snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else g_snprintf(text, sizeof text, "%zu KB", (bytes + 1023) / 1024);
    return text;
}

// Decodes image bytes into a texture, which both previews the cover and proves the image is readable.
GdkTexture* decode_image(const std::vector<std::uint8_t>& data) {
    std::unique_ptr<GBytes, decltype(&g_bytes_unref)> bytes(g_bytes_new(data.data(), data.size()), g_bytes_unref);
    GError* error = nullptr;
    auto* texture = gdk_texture_new_from_bytes(bytes.get(), &error);
    if (!texture) {
        const std::string message = error && error->message ? error->message : "unknown format";
        g_clear_error(&error);
        throw std::runtime_error("This image could not be decoded: " + message);
    }
    return texture;
}

std::string duration(int seconds) {
    return std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
}
}

Window::Window(GtkApplication* application, std::filesystem::path config,
               std::vector<std::filesystem::path> paths) : config_(std::filesystem::absolute(config).lexically_normal()) {
    try {
        ensure_theme(config_);
        theme_ = sync_theme(config_);
        settings_file_ = settings_path(config_);
        settings_ = load_settings(settings_file_);
        window_ = gtk_application_window_new(application);
        g_object_ref(window_);
        display_ = gdk_display_get_default();
        css_ = gtk_css_provider_new();
        g_signal_connect(css_, "parsing-error", G_CALLBACK(+[](GtkCssProvider*, GtkCssSection* section, GError* error, gpointer) {
            auto* start = gtk_css_section_get_start_location(section);
            std::cerr << "MetaDots CSS " << (start ? start->lines + 1 : 0) << ": " << error->message << '\n';
        }), nullptr);
        gtk_style_context_add_provider_for_display(display_, GTK_STYLE_PROVIDER(css_), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        build();
        apply_theme(theme_);
        open_paths(paths, false);
        watch_theme();
        timer_ = g_timeout_add(500, +[](gpointer data) -> gboolean {
            auto& self = *static_cast<Window*>(data);
            if (self.closing_) { self.timer_ = 0; return G_SOURCE_REMOVE; }
            self.guarded([&] { self.refresh_theme(); });
            return G_SOURCE_CONTINUE;
        }, this);
    } catch (...) {
        cleanup();
        throw;
    }
}

Window::~Window() { cleanup(); }

void Window::close_window() noexcept {
    closing_ = true;
    if (picker_cancel_) g_cancellable_cancel(picker_cancel_);
    if (motion_tick_ && window_ && !destroyed_) gtk_widget_remove_tick_callback(window_, motion_tick_);
    motion_tick_ = 0;
    if (theme_monitor_) {
        if (theme_monitor_handler_) {
            g_signal_handler_disconnect(theme_monitor_, theme_monitor_handler_);
            theme_monitor_handler_ = 0;
        }
        g_file_monitor_cancel(theme_monitor_);
    }
    if (theme_debounce_) { g_source_remove(theme_debounce_); theme_debounce_ = 0; }
    if (timer_) { g_source_remove(timer_); timer_ = 0; }
    g_clear_object(&theme_monitor_);
    g_clear_object(&theme_file_);
    if (window_ && !destroyed_) {
        destroyed_ = true;
        gtk_window_destroy(GTK_WINDOW(window_));
    }
}

void Window::cleanup() noexcept {
    close_window();
    g_clear_object(&cover_texture_);
    g_clear_object(&picker_cancel_);
    if (window_) {
        g_object_unref(window_);
        window_ = nullptr;
    }
    if (css_) {
        gtk_style_context_remove_provider_for_display(display_, GTK_STYLE_PROVIDER(css_));
        g_object_unref(css_);
        css_ = nullptr;
    }
}

void Window::present() { gtk_window_present(GTK_WINDOW(window_)); }

void Window::guarded(const std::function<void()>& action) noexcept {
    if (closing_) return;
    try { action(); }
    catch (const std::exception& error) { show_error(error.what()); }
    catch (...) { show_error("The operation could not be completed."); }
}

void Window::show_error(const char* message) noexcept {
    if (closing_) return;
    if (status_) gtk_label_set_text(GTK_LABEL(status_), message);
    if (blur_) metadots_blur_bin_invalidate(blur_);
    if (modal_feedback_) {
        gtk_label_set_text(GTK_LABEL(modal_feedback_), message);
        gtk_widget_set_visible(modal_feedback_, true);
    }
}

void Window::connect_action(GtkWidget* widget, const char* signal, std::function<void()> action) {
    struct Callback { Window* owner; std::function<void()> action; };
    auto* closure = new Callback{this, std::move(action)};
    g_signal_connect_data(widget, signal, G_CALLBACK(+[](gpointer, gpointer data) {
        auto* callback = static_cast<Callback*>(data);
        callback->owner->guarded([callback] {
            // A modal action can destroy its own button and signal closure.
            auto invoke = callback->action;
            invoke();
        });
    }), closure, +[](gpointer data, GClosure*) {
        delete static_cast<Callback*>(data);
    }, G_CONNECT_DEFAULT);
}

GtkWidget* Window::button(const char* text, const char* icon, std::function<void()> action) {
    auto* widget = gtk_button_new();
    auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    if (icon) gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
    if (text && *text) gtk_box_append(GTK_BOX(box), label(text));
    gtk_button_set_child(GTK_BUTTON(widget), box);
    gtk_accessible_update_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
    connect_action(widget, "clicked", std::move(action));
    return widget;
}

void Window::build() {
    gtk_window_set_title(GTK_WINDOW(window_), "MetaDots");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1120, 740);
    gtk_widget_set_size_request(window_, 760, 540);
    gtk_widget_add_css_class(window_, "metadots");
    // The in-content header replaces the system titlebar; keep an empty one that takes no space.
    auto* titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(titlebar, "no-titlebar");
    gtk_window_set_titlebar(GTK_WINDOW(window_), titlebar);
    content_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(content_), build_header());
    auto* split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(split), build_library());
    gtk_paned_set_end_child(GTK_PANED(split), build_editor());
    gtk_paned_set_position(GTK_PANED(split), 480);
    gtk_paned_set_resize_start_child(GTK_PANED(split), true);
    gtk_paned_set_shrink_start_child(GTK_PANED(split), false);
    gtk_paned_set_shrink_end_child(GTK_PANED(split), false);
    gtk_widget_set_vexpand(split, true);
    gtk_box_append(GTK_BOX(content_), split);
    status_ = label("Open audio files or a folder to begin.", "status");
    gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(content_), status_);
    blur_ = metadots_blur_bin_new(content_);
    overlay_ = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay_), blur_);
    gtk_window_set_child(GTK_WINDOW(window_), overlay_);
    g_signal_connect(window_, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
        auto& self = *static_cast<Window*>(data);
        self.guarded([&] { self.request_close(); });
        return true;
    }), this);
    setup_keyboard();
    setup_drop();
}

void Window::setup_keyboard() {
    auto* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint,
                                                         GdkModifierType state, gpointer data) -> gboolean {
        auto& self = *static_cast<Window*>(data);
        if (key == GDK_KEY_Menu || ((state & GDK_SHIFT_MASK) && key == GDK_KEY_F10)) return true;
        if (self.modal_) {
            if (key == GDK_KEY_Escape) self.dismiss_modal();
            return key == GDK_KEY_Escape;
        }
        if (!(state & GDK_CONTROL_MASK)) return false;
        bool handled = true;
        self.guarded([&] {
            switch (gdk_keyval_to_lower(key)) {
                case GDK_KEY_o: self.request_open(); break;
                case GDK_KEY_s: self.request_save(); break;
                case GDK_KEY_a: {
                    auto* focus = gtk_root_get_focus(GTK_ROOT(self.window_));
                    if (focus && (focus == self.files_ || gtk_widget_is_ancestor(focus, self.files_)))
                        gtk_list_box_select_all(GTK_LIST_BOX(self.files_));
                    else handled = false;
                    break;
                }
                case GDK_KEY_f: gtk_widget_grab_focus(self.search_); break;
                default: handled = false;
            }
        });
        return handled;
    }), this);
    gtk_widget_add_controller(window_, keys);
    auto* context = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context), 3);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(context), GTK_PHASE_CAPTURE);
    g_signal_connect(context, "pressed", G_CALLBACK(+[](GtkGestureClick* gesture, int, double, double, gpointer) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }), nullptr);
    gtk_widget_add_controller(window_, GTK_EVENT_CONTROLLER(context));
}

void Window::setup_drop() {
    auto* drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(+[](GtkDropTarget*, const GValue* received, double, double, gpointer data) -> gboolean {
        auto& self = *static_cast<Window*>(data);
        if (self.modal_) return false;
        self.guarded([&] {
            std::vector<std::filesystem::path> paths;
            auto* list = static_cast<GdkFileList*>(g_value_get_boxed(received));
            if (!list) return;
            for (auto* item = gdk_file_list_get_files(list); item; item = item->next) {
                std::unique_ptr<char, decltype(&g_free)> path(g_file_get_path(G_FILE(item->data)), g_free);
                if (path) paths.emplace_back(path.get());
            }
            self.open_paths(paths, false);
        });
        return true;
    }), this);
    gtk_widget_add_controller(window_, GTK_EVENT_CONTROLLER(drop));
}

GtkWidget* Window::build_header() {
    auto* header = gtk_header_bar_new();
    auto* brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    auto* mark = gtk_image_new_from_icon_name("audio-x-generic-symbolic");
    gtk_widget_add_css_class(mark, "brand-icon");
    gtk_box_append(GTK_BOX(brand), mark);
    gtk_box_append(GTK_BOX(brand), label("MetaDots", "brand"));
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), brand);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), button("Open", "folder-open-symbolic", [this] { request_open(); }));
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), button("Settings", "preferences-system-symbolic", [this] { open_settings(); }));
    save_ = button("Save changes", "document-save-symbolic", [this] { request_save(); });
    gtk_widget_add_css_class(save_, "suggested-action");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), save_);
    return header;
}

GtkWidget* Window::build_library() {
    auto* library = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(library, "library");
    gtk_widget_set_size_request(library, 300, -1);
    auto* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* title = label("Audio files", "section-title");
    gtk_widget_set_hexpand(title, true);
    gtk_box_append(GTK_BOX(top), title);
    gtk_box_append(GTK_BOX(top), button("Select all", nullptr, [this] { gtk_list_box_select_all(GTK_LIST_BOX(files_)); }));
    gtk_box_append(GTK_BOX(library), top);
    search_ = gtk_search_entry_new();
    g_object_set(search_, "placeholder-text", "Filter title, artist, album or filename", nullptr);
    connect_action(search_, "search-changed", [this] { filter_files(); });
    gtk_box_append(GTK_BOX(library), search_);
    files_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(files_), GTK_SELECTION_MULTIPLE);
    // Single-click activation would bypass the Ctrl/Shift handling that builds a group.
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(files_), false);
    auto* empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    margins(empty, 30);
    auto* icon = gtk_image_new_from_icon_name("folder-music-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 44);
    gtk_widget_add_css_class(icon, "muted");
    gtk_box_append(GTK_BOX(empty), icon);
    auto* hint = label("Your music, clearly tagged", "section-title");
    gtk_label_set_xalign(GTK_LABEL(hint), .5);
    gtk_box_append(GTK_BOX(empty), hint);
    auto* description = label("Open a folder or drop audio files here.\nSelect several tracks to edit them together.", "muted");
    gtk_label_set_wrap(GTK_LABEL(description), true);
    gtk_label_set_xalign(GTK_LABEL(description), .5);
    gtk_label_set_justify(GTK_LABEL(description), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(empty), description);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(files_), empty);
    connect_action(files_, "selected-rows-changed", [this] {
        if (updating_) return;
        const auto carried = apply_carry();
        refresh_editor();
        if (!carried.empty()) set_status(carried);
    });
    gtk_box_append(GTK_BOX(library), scroll(files_));
    gtk_box_append(GTK_BOX(library), label("Ctrl + click to select multiple files", "muted"));
    return library;
}

GtkWidget* Window::build_cover() {
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_bottom(row, 10);
    cover_frame_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(cover_frame_, "cover-frame");
    gtk_widget_set_size_request(cover_frame_, 104, 104);
    gtk_widget_set_valign(cover_frame_, GTK_ALIGN_START);
    gtk_widget_set_halign(cover_frame_, GTK_ALIGN_START);
    gtk_widget_set_overflow(cover_frame_, GTK_OVERFLOW_HIDDEN);
    gtk_accessible_update_property(GTK_ACCESSIBLE(cover_frame_), GTK_ACCESSIBLE_PROPERTY_LABEL, "Cover image", -1);
    gtk_widget_set_hexpand(cover_frame_, false);
    cover_stack_ = gtk_stack_new();
    // A fixed pixel size keeps a large image from stretching the editor column.
    cover_picture_ = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(cover_picture_), 102);
    gtk_accessible_update_property(GTK_ACCESSIBLE(cover_picture_), GTK_ACCESSIBLE_PROPERTY_LABEL, "Cover image", -1);
    gtk_stack_add_named(GTK_STACK(cover_stack_), cover_picture_, "art");
    cover_note_ = label("No cover", "muted");
    gtk_widget_add_css_class(cover_note_, "cover-note");
    gtk_label_set_xalign(GTK_LABEL(cover_note_), .5);
    gtk_label_set_justify(GTK_LABEL(cover_note_), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(cover_note_), true);
    gtk_widget_set_halign(cover_note_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(cover_note_, GTK_ALIGN_CENTER);
    gtk_stack_add_named(GTK_STACK(cover_stack_), cover_note_, "note");
    gtk_box_append(GTK_BOX(cover_frame_), cover_stack_);
    gtk_box_append(GTK_BOX(row), cover_frame_);

    // Dropping one JPEG or PNG on the cover sets it; anything else dropped here is opened as audio, like the window does.
    auto* drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "accept", G_CALLBACK(+[](GtkDropTarget*, GdkDrop*, gpointer data) -> gboolean {
        return !static_cast<Window*>(data)->modal_;
    }), this);
    g_signal_connect(drop, "enter", G_CALLBACK(+[](GtkDropTarget*, double, double, gpointer data) -> GdkDragAction {
        gtk_widget_add_css_class(static_cast<Window*>(data)->cover_frame_, "drop-active");
        return GDK_ACTION_COPY;
    }), this);
    g_signal_connect(drop, "leave", G_CALLBACK(+[](GtkDropTarget*, gpointer data) {
        gtk_widget_remove_css_class(static_cast<Window*>(data)->cover_frame_, "drop-active");
    }), this);
    g_signal_connect(drop, "drop", G_CALLBACK((+[](GtkDropTarget*, const GValue* received, double, double, gpointer data) -> gboolean {
        auto& self = *static_cast<Window*>(data);
        gtk_widget_remove_css_class(self.cover_frame_, "drop-active");
        auto* list = static_cast<GdkFileList*>(g_value_get_boxed(received));
        if (!list || self.modal_) return false;
        std::vector<std::filesystem::path> paths;
        for (auto* item = gdk_file_list_get_files(list); item; item = item->next) {
            std::unique_ptr<char, decltype(&g_free)> path(g_file_get_path(G_FILE(item->data)), g_free);
            if (path) paths.emplace_back(path.get());
        }
        // A drop that this target accepts never reaches the window's target, so audio is opened here too.
        self.guarded([&] {
            if (paths.size() == 1 && is_image_path(paths.front())) self.set_cover_from_file(paths.front());
            else self.open_paths(paths, false);
        });
        return true;
    })), this);
    gtk_widget_add_controller(cover_frame_, GTK_EVENT_CONTROLLER(drop));

    auto* side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(side, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(side), button("Choose image…", nullptr, [this] { choose_cover(); }));
    cover_remove_ = button("Remove cover", nullptr, [this] { remove_cover(); });
    gtk_box_append(GTK_BOX(side), cover_remove_);
    cover_info_ = label("", "muted");
    gtk_label_set_wrap(GTK_LABEL(cover_info_), true);
    gtk_label_set_max_width_chars(GTK_LABEL(cover_info_), 22);
    gtk_box_append(GTK_BOX(side), cover_info_);
    gtk_box_append(GTK_BOX(row), side);
    return row;
}

void Window::refresh_cover() {
    if (!cover_stack_) return;
    const auto indices = selected();
    const auto show_note = [&](const char* text, const std::string& info) {
        gtk_label_set_text(GTK_LABEL(cover_note_), text);
        gtk_stack_set_visible_child_name(GTK_STACK(cover_stack_), "note");
        gtk_label_set_text(GTK_LABEL(cover_info_), info.c_str());
    };
    bool any_cover = false, mixed = false;
    for (const auto index : indices) {
        const auto hash = documents_[index].cover_hash();
        any_cover = any_cover || hash != 0;
        mixed = mixed || hash != documents_[indices.front()].cover_hash();
    }
    gtk_widget_set_sensitive(cover_remove_, any_cover);
    if (indices.empty()) { show_note("No file selected", ""); return; }
    if (mixed) { show_note("Mixed covers", "Choose an image to give every selected file the same cover."); return; }
    const auto& document = documents_[indices.front()];
    if (!document.cover_hash()) { show_note("No cover", "Choose a JPEG or PNG image."); return; }
    try {
        if (!cover_texture_ || cover_texture_hash_ != document.cover_hash()) {
            std::optional<Cover> stored;
            const Cover* cover = nullptr;
            if (document.cover_edit && document.cover_edit->image) cover = document.cover_edit->image.get();
            else if ((stored = read_cover(document.track.path))) cover = &*stored;
            if (!cover) throw std::runtime_error("The cover could not be read from the file.");
            auto* texture = decode_image(cover->data);
            g_clear_object(&cover_texture_);
            cover_texture_ = texture;
            cover_texture_hash_ = document.cover_hash();
            cover_texture_mime_ = cover->mime;
            cover_texture_bytes_ = cover->data.size();
        }
        gtk_image_set_from_paintable(GTK_IMAGE(cover_picture_), GDK_PAINTABLE(cover_texture_));
        gtk_stack_set_visible_child_name(GTK_STACK(cover_stack_), "art");
        const auto slash = cover_texture_mime_.find('/');
        std::string format = slash == std::string::npos ? cover_texture_mime_ : cover_texture_mime_.substr(slash + 1);
        std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        std::string info = format + std::string(", ") +
                           std::to_string(gdk_texture_get_width(cover_texture_)) + " × " + std::to_string(gdk_texture_get_height(cover_texture_)) +
                           ", " + size_text(cover_texture_bytes_);
        if (document.cover_edit) info += ". Not saved yet.";
        gtk_label_set_text(GTK_LABEL(cover_info_), info.c_str());
    } catch (const std::exception& error) {
        show_note("Cover can't be shown", error.what());
    }
}

void Window::choose_cover() {
    if (selected().empty()) return;
    if (settings_.use_native_picker) open_browser(true);
    else system_pick(Pick::Image);
}

void Window::remove_cover() {
    stage_cover(CoverChange{});
}

void Window::set_cover_from_file(const std::filesystem::path& path) {
    if (selected().empty()) throw std::runtime_error("Select a file before choosing a cover.");
    // Validate first so a bad image is reported inside the browser instead of closing it.
    auto cover = load_cover_file(path);
    auto* texture = decode_image(cover.data);
    dismiss_modal();
    g_clear_object(&cover_texture_);
    cover_texture_ = texture;
    cover_texture_hash_ = cover_fingerprint(cover.data);
    cover_texture_mime_ = cover.mime;
    cover_texture_bytes_ = cover.data.size();
    stage_cover(CoverChange{std::make_shared<const Cover>(std::move(cover))});
}

void Window::stage_cover(const CoverChange& change) {
    const auto indices = selected();
    if (indices.empty()) throw std::runtime_error("Select a file before changing the cover.");
    const auto hash = change.image ? cover_fingerprint(change.image->data) : 0;
    for (const auto index : indices) {
        auto& document = documents_[index];
        if (hash == document.track.cover_hash) { document.cover_edit.reset(); document.cover_edit_hash = 0; }
        else { document.cover_edit = change; document.cover_edit_hash = hash; }
    }
    // Like other shared edits, the cover follows you to the next file you open.
    carry_cover_ = change;
    carry_cover_hash_ = hash;
    ++carry_generation_;
    refresh_cover();
    refresh_dirty();
    std::string message = change.image ? "Cover set on " + count_of(indices.size(), "file") + ". Save changes to write it."
                                       : "Cover will be removed from " + count_of(indices.size(), "file") + ". Save changes to write it.";
    if (change.image && change.image->data.size() > 2 * 1024 * 1024)
        message += " This image is " + size_text(change.image->data.size()) + "; some players are slow with large covers.";
    set_status(message);
}

GtkWidget* Window::build_editor() {
    auto* wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(wrapper, "inspector");
    gtk_widget_set_size_request(wrapper, 330, -1);
    selection_label_ = label("Track details", "card-title");
    gtk_label_set_ellipsize(GTK_LABEL(selection_label_), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(wrapper), selection_label_);
    details_ = label("Select a file to inspect its metadata.", "muted");
    gtk_label_set_wrap(GTK_LABEL(details_), true);
    gtk_box_append(GTK_BOX(wrapper), details_);
    editor_ = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(editor_), 8);
    gtk_grid_set_column_spacing(GTK_GRID(editor_), 14);
    gtk_widget_set_valign(editor_, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(editor_), build_cover(), 0, 0, 2, 1);
    int row = 1;
    for (const auto& [key, caption] : tag_fields) {
        auto* entry = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(entry), 8192);
        gtk_widget_set_hexpand(entry, true);
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY, "edit-clear-symbolic");
        gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                       "The clear icon removes this tag from every selected file", -1);
        g_object_set_data_full(G_OBJECT(entry), "tag-key", g_strdup(key), g_free);
        g_signal_connect(entry, "icon-release", G_CALLBACK(+[](GtkEntry* activated, GtkEntryIconPosition position, gpointer data) {
            if (position != GTK_ENTRY_ICON_SECONDARY) return;
            auto& self = *static_cast<Window*>(data);
            self.guarded([&] {
                const auto* tag = static_cast<const char*>(g_object_get_data(G_OBJECT(activated), "tag-key"));
                if (tag) self.clear_field(tag);
            });
        }), this);
        fields_[key] = entry;
        connect_action(entry, "changed", [this, name = std::string(key)] { if (!updating_) edit_field(name); });
        auto* name = label(caption, "field-label");
        gtk_widget_set_valign(name, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(editor_), name, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(editor_), entry, 1, row, 1, 1);
        ++row;
        if (std::string_view(key) == "title") {
            explicit_toggle_ = gtk_check_button_new_with_label("Explicit");
            gtk_widget_set_tooltip_text(explicit_toggle_, "Adds or removes the boxed E that Apple Music shows after explicit titles (a 🅴 character at the end of the title)");
            connect_action(explicit_toggle_, "toggled", [this] { if (!updating_) toggle_explicit(); });
            gtk_grid_attach(GTK_GRID(editor_), explicit_toggle_, 1, row, 1, 1);
            ++row;
        }
    }
    gtk_box_append(GTK_BOX(wrapper), scroll(editor_));
    carry_toggle_ = gtk_check_button_new_with_label("Copy my last edits to the next file");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(carry_toggle_), true);
    gtk_widget_set_tooltip_text(carry_toggle_, "Artist, album, cover and other shared edits you just made are filled in when you open another file. Title and track number are never copied.");
    gtk_box_append(GTK_BOX(wrapper), carry_toggle_);
    gtk_box_append(GTK_BOX(wrapper), button("Revert selected edits", "edit-undo-symbolic", [this] { request_revert(); }));
    gtk_widget_set_sensitive(editor_, false);
    return wrapper;
}

void Window::set_status(const std::string& message) {
    if (status_ && !closing_) gtk_label_set_text(GTK_LABEL(status_), message.c_str());
    if (blur_) metadots_blur_bin_invalidate(blur_);
}

void Window::apply_theme(const Theme& theme) {
    std::ostringstream css;
    const auto color = [&](const std::string& key) { return color_css(theme.colors.at(key)); };
    // Tones are overlays of the text color, so they hold up on light, dark and translucent palettes.
    const auto tone = [&](const char* amount) { return "alpha(" + color("text") + ", " + amount + ")"; };
    const auto rule = "alpha(" + color("border") + ", .45)";
    const std::string hand = "'Grape Nuts', 'Caveat', 'Noto Sans', sans-serif";
    css << ".metadots { background: transparent; color: " << color("text") << "; font-family: 'Inter', 'Noto Sans', 'Adwaita Sans', sans-serif; font-size: 14px; }\n"
        // Clear the base theme's bare box/text/label paint so only the rules below apply.
        << ".metadots box, .metadots label, .metadots image, .metadots stack { background: none; }\n"
        << ".metadots .no-titlebar { min-height: 0; min-width: 0; padding: 0; margin: 0; border: none; background: none; box-shadow: none; }\n"
        << ".metadots text { background: none; border: none; border-radius: 0; box-shadow: none; padding: 0; min-height: 0; }\n"
        << ".metadots headerbar { background: " << color("background") << "; color: " << color("text") << "; border: none; border-bottom: 1px dashed " << rule << "; box-shadow: none; min-height: 0; padding: 10px 14px; }\n"
        << ".metadots .brand { font-family: " << hand << "; font-size: 24px; font-weight: 400; }\n"
        << ".metadots .brand-icon { color: " << color("accent") << "; }\n"
        << ".metadots .library { background: " << color("background") << "; padding: 20px 18px 14px 20px; }\n"
        << ".metadots .inspector { background: " << color("surface_alt") << "; border-left: 1px dashed " << rule << "; padding: 22px 26px 18px 26px; }\n"
        << ".metadots .section-title { font-family: " << hand << "; font-size: 23px; font-weight: 400; }\n"
        << ".metadots .card-title { font-family: " << hand << "; font-size: 26px; font-weight: 400; }\n"
        << ".metadots .muted { color: " << tone(".6") << "; font-size: 13px; }\n"
        << ".metadots checkbutton { color: " << tone(".78") << "; font-size: 13px; }\n"
        << ".metadots checkbutton check { background: none; border: 1px solid " << tone(".5") << "; border-radius: 3px; color: " << color("accent_text") << "; box-shadow: none; -gtk-icon-shadow: none; min-width: 16px; min-height: 16px; }\n"
        << ".metadots checkbutton check:checked { background: " << color("accent") << "; border-color: " << color("accent") << "; }\n"
        << ".metadots checkbutton:focus-visible check { box-shadow: 0 0 0 2px alpha(" << color("accent") << ", .55); }\n"
        << ".metadots .cover-frame { background: " << tone(".06") << "; border: 1px dashed " << rule << "; border-radius: 8px; }\n"
        << ".metadots .cover-frame.drop-active { border-color: " << color("accent") << "; background: alpha(" << color("accent") << ", .16); }\n"
        << ".metadots .cover-note { font-size: 12px; padding: 6px; }\n"
        << ".metadots .field-label { color: " << tone(".68") << "; font-size: 13px; min-width: 86px; }\n"
        << ".metadots .duration { color: " << tone(".6") << "; font-size: 13px; font-feature-settings: 'tnum'; }\n"
        << ".metadots entry, .metadots searchentry { background: " << tone(".07") << "; color: " << color("text") << "; border: 1px solid transparent; border-radius: 10px; padding: 7px 11px; min-height: 22px; caret-color: " << color("accent") << "; }\n"
        << ".metadots entry text, .metadots searchentry text { color: " << color("text") << "; }\n"
        << ".metadots entry text selection, .metadots searchentry text selection { background-color: alpha(" << color("accent") << ", .38); color: " << color("text") << "; }\n"
        << ".metadots entry text placeholder, .metadots searchentry text placeholder { color: " << tone(".5") << "; }\n"
        << ".metadots entry image, .metadots searchentry image { color: " << tone(".5") << "; }\n"
        << ".metadots entry image:hover { color: " << color("text") << "; }\n"
        << ".metadots entry:focus-within, .metadots searchentry:focus-within { outline: none; box-shadow: inset 0 0 0 2px " << color("accent") << "; }\n"
        // J-card: fields are ruled lines you write on, not boxes.
        << ".metadots .inspector entry { background: none; border: none; border-bottom: 1px solid " << rule << "; border-radius: 0; padding: 6px 2px 5px 2px; }\n"
        << ".metadots .inspector entry:hover { background: " << tone(".04") << "; }\n"
        << ".metadots .inspector entry:focus-within { background: " << tone(".05") << "; box-shadow: inset 0 -2px " << color("accent") << "; }\n"
        << ".metadots .inspector entry:disabled { opacity: .5; }\n"
        << ".metadots button { background: " << tone(".08") << "; color: " << color("text") << "; border: 1px solid transparent; border-radius: 10px; padding: 6px 14px; min-height: 22px; }\n"
        << ".metadots button:hover { background: " << tone(".15") << "; }\n"
        << ".metadots button:active { background: " << tone(".2") << "; }\n"
        << ".metadots button.suggested-action { background: " << color("accent") << "; color: " << color("accent_text") << "; font-weight: 600; }\n"
        << ".metadots button.suggested-action:hover { background: alpha(" << color("accent") << ", .85); }\n"
        << ".metadots button.destructive { background: alpha(" << color("error") << ", .14); color: " << color("error") << "; }\n"
        << ".metadots button.destructive:hover { background: alpha(" << color("error") << ", .24); }\n"
        << ".metadots button:disabled { opacity: .4; }\n"
        << ".metadots button:focus-visible { outline: 2px solid " << color("accent") << "; outline-offset: 2px; }\n"
        << ".metadots windowcontrols button { padding: 6px 9px; }\n"
        << ".metadots list { background: transparent; color: " << color("text") << "; }\n"
        << ".metadots list row { background: transparent; color: " << color("text") << "; border-left: 3px solid transparent; border-radius: 6px; padding: 10px 12px; margin: 0 0 4px 0; }\n"
        << ".metadots list row:hover { background: " << tone(".06") << "; }\n"
        << ".metadots list row:selected { background: " << color("selection") << "; color: " << color("text") << "; border-left-color: " << color("accent") << "; }\n"
        << ".metadots list row:focus-visible { outline: 2px solid " << color("accent") << "; outline-offset: -2px; }\n"
        << ".metadots .track-title { font-weight: 600; }\n"
        << ".metadots .dirty { color: " << color("accent") << "; font-size: 15px; }\n"
        << ".metadots .status { background: " << color("background") << "; color: " << tone(".66") << "; border-top: 1px dashed " << rule << "; padding: 8px 20px; font-size: 13px; }\n"
        << ".metadots paned > separator { background-color: " << color("background") << "; background-image: none; min-width: 1px; border: none; }\n"
        << ".metadots .modal-shade { background: rgba(0, 0, 0, .45); }\n"
        << ".metadots .modal-card { background: " << color("surface_alt") << "; border: 1px solid " << rule << "; border-radius: 16px; padding: 26px 28px; }\n"
        << ".metadots .modal-title { font-family: " << hand << "; font-size: 27px; font-weight: 400; }\n"
        << ".metadots .modal-feedback { color: " << color("error") << "; }\n"
        << ".metadots scrollbar { background: transparent; } .metadots scrollbar slider { background: " << tone(".22") << "; border-radius: 5px; min-width: 5px; min-height: 28px; }\n"
        << ".metadots scrollbar.hovering slider { background: " << tone(".42") << "; }\n";
    gtk_css_provider_load_from_string(css_, css.str().c_str());
    if (blur_) metadots_blur_bin_invalidate(blur_);
    theme_ = theme;
}

void Window::refresh_theme() {
    try {
        const auto candidate = sync_theme(config_);
        if (candidate.colors != theme_.colors || candidate.name != theme_.name) apply_theme(candidate);
        last_theme_error_.clear();
    } catch (const std::exception& error) {
        if (last_theme_error_ != error.what()) {
            last_theme_error_ = error.what();
            set_status("Theme was not reloaded: " + last_theme_error_);
        }
    }
}

void Window::watch_theme() {
    theme_file_ = g_file_new_for_path(config_.c_str());
    std::unique_ptr<GFile, decltype(&g_object_unref)> directory(g_file_get_parent(theme_file_), g_object_unref);
    if (!directory) return;
    GError* error = nullptr;
    theme_monitor_ = g_file_monitor_directory(directory.get(), G_FILE_MONITOR_WATCH_MOVES, nullptr, &error);
    if (!theme_monitor_) {
        if (error) { show_error(error->message); g_error_free(error); }
        return;
    }
    g_file_monitor_set_rate_limit(theme_monitor_, 25);
    theme_monitor_handler_ = g_signal_connect(theme_monitor_, "changed", G_CALLBACK(+[](GFileMonitor*, GFile* file, GFile* other, GFileMonitorEvent, gpointer data) {
        auto& self = *static_cast<Window*>(data);
        if (self.closing_) return;
        if ((file && g_file_equal(file, self.theme_file_)) ||
            (other && g_file_equal(other, self.theme_file_))) self.queue_theme_reload();
    }), this);
}

void Window::queue_theme_reload() {
    if (closing_) return;
    if (theme_debounce_) g_source_remove(theme_debounce_);
    theme_debounce_ = g_timeout_add(25, +[](gpointer data) -> gboolean {
        auto& self = *static_cast<Window*>(data);
        self.theme_debounce_ = 0;
        self.guarded([&] { self.refresh_theme(); });
        return G_SOURCE_REMOVE;
    }, this);
}

void Window::open_paths(const std::vector<std::filesystem::path>& paths, bool recursive) {
    std::set<std::filesystem::path> known;
    for (const auto& document : documents_) known.insert(document.track.path);
    std::size_t added = 0, failed = 0;
    std::string last_error;
    for (const auto& path : paths) {
        std::vector<std::filesystem::path> candidates;
        try {
            if (std::filesystem::is_regular_file(std::filesystem::symlink_status(path))) candidates.push_back(path);
            else candidates = scan_audio(path, recursive);
        }
        catch (const std::exception& error) { ++failed; last_error = error.what(); continue; }
        for (const auto& candidate : candidates) {
            if (documents_.size() >= 10000) { last_error = "The 10,000-file workspace limit was reached."; ++failed; break; }
            const auto absolute = std::filesystem::absolute(candidate).lexically_normal();
            if (known.contains(absolute)) continue;
            try {
                auto track = read_track(absolute);
                Document document;
                document.track = std::move(track);
                documents_.push_back(std::move(document));
                known.insert(absolute);
                ++added;
            } catch (const std::exception& error) { ++failed; last_error = error.what(); }
        }
    }
    rebuild_files();
    if (added || failed) set_status(count_of(added, "file") + " added" + (failed ? "; " + std::to_string(failed) + " skipped. " + last_error : "."));
}

void Window::rebuild_files() {
    const auto selection = selected();
    updating_ = true;
    gtk_list_box_remove_all(GTK_LIST_BOX(files_));
    for (std::size_t index = 0; index < documents_.size(); ++index) {
        const auto& document = documents_[index];
        auto* row = gtk_list_box_row_new();
        g_object_set_data(G_OBJECT(row), "document-index", GSIZE_TO_POINTER(index + 1));
        auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("audio-x-generic-symbolic"));
        auto* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_widget_set_hexpand(text, true);
        const auto title = value(document, "title");
        auto* name = label(title.empty() ? document.track.path.filename().string() : title, "track-title");
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(text), name);
        const auto artist = value(document, "artist");
        auto* subtitle = label((artist.empty() ? "Unknown artist" : artist) + " · " + document.track.path.filename().string(), "muted");
        gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(text), subtitle);
        gtk_box_append(GTK_BOX(box), text);
        auto* end = label(!document.dirty() ? duration(document.track.duration_seconds) : "●", !document.dirty() ? "duration" : "dirty");
        g_object_set_data(G_OBJECT(row), "dirty-indicator", end);
        gtk_box_append(GTK_BOX(box), end);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(GTK_LIST_BOX(files_), row);
        if (std::find(selection.begin(), selection.end(), index) != selection.end()) gtk_list_box_select_row(GTK_LIST_BOX(files_), GTK_LIST_BOX_ROW(row));
    }
    updating_ = false;
    filter_files();
    if (selection.empty() && !documents_.empty()) gtk_list_box_select_row(GTK_LIST_BOX(files_), gtk_list_box_get_row_at_index(GTK_LIST_BOX(files_), 0));
    refresh_editor();
}

void Window::filter_files() {
    const auto query = folded(gtk_editable_get_text(GTK_EDITABLE(search_)));
    for (auto* row = gtk_widget_get_first_child(files_); row; row = gtk_widget_get_next_sibling(row)) {
        if (!GTK_IS_LIST_BOX_ROW(row)) continue;
        const auto index = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "document-index")) - 1;
        if (index >= documents_.size()) continue;
        const auto& document = documents_[index];
        std::string searchable = document.track.path.string();
        for (const auto& [key, caption] : tag_fields) { (void)caption; searchable += " " + value(document, key); }
        const bool visible = folded(searchable).find(query) != std::string::npos;
        if (!visible) gtk_list_box_unselect_row(GTK_LIST_BOX(files_), GTK_LIST_BOX_ROW(row));
        gtk_widget_set_visible(row, visible);
    }
}

std::vector<std::size_t> Window::selected() const {
    std::vector<std::size_t> indices;
    if (!files_) return indices;
    std::unique_ptr<GList, decltype(&g_list_free)> rows(gtk_list_box_get_selected_rows(GTK_LIST_BOX(files_)), g_list_free);
    for (auto* row = rows.get(); row; row = row->next) {
        const auto index = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row->data), "document-index")) - 1;
        if (index < documents_.size()) indices.push_back(index);
    }
    return indices;
}

void Window::refresh_editor() {
    const auto indices = selected();
    updating_ = true;
    gtk_widget_set_sensitive(editor_, !indices.empty());
    std::string heading = "Track details";
    if (indices.size() > 1) heading = std::to_string(indices.size()) + " tracks selected";
    else if (indices.size() == 1) {
        const auto& document = documents_[indices.front()];
        const auto title = value(document, "title");
        heading = title.empty() ? document.track.path.filename().string() : title;
    }
    gtk_label_set_text(GTK_LABEL(selection_label_), heading.c_str());
    for (const auto& [key, entry] : fields_) {
        const auto first = indices.empty() ? "" : value(documents_[indices.front()], key);
        const bool mixed = std::any_of(indices.begin(), indices.end(), [&](std::size_t index) { return value(documents_[index], key) != first; });
        gtk_editable_set_text(GTK_EDITABLE(entry), mixed ? "" : first.c_str());
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), mixed ? "Mixed values. Edit to apply to all." : "");
    }
    if (indices.size() == 1) {
        const auto& track = documents_[indices.front()].track;
        const auto text = track.format + " · " + duration(track.duration_seconds) + " · " + std::to_string(track.bitrate_kbps) + " kbps · " + std::to_string(track.sample_rate) + " Hz";
        gtk_label_set_text(GTK_LABEL(details_), text.c_str());
    } else gtk_label_set_text(GTK_LABEL(details_), indices.empty() ? "Select a file to inspect its metadata." : "Only fields you change are applied to every selected file.");
    refresh_explicit();
    updating_ = false;
    refresh_cover();
    refresh_dirty();
}

void Window::refresh_explicit() {
    const auto indices = selected();
    const auto count = std::count_if(indices.begin(), indices.end(), [&](std::size_t index) { return has_explicit(value(documents_[index], "title")); });
    const bool was_updating = updating_;
    updating_ = true;
    gtk_check_button_set_inconsistent(GTK_CHECK_BUTTON(explicit_toggle_), false);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(explicit_toggle_), !indices.empty() && count == static_cast<std::ptrdiff_t>(indices.size()));
    gtk_check_button_set_inconsistent(GTK_CHECK_BUTTON(explicit_toggle_), count != 0 && count != static_cast<std::ptrdiff_t>(indices.size()));
    updating_ = was_updating;
}

void Window::toggle_explicit() {
    const bool on = gtk_check_button_get_active(GTK_CHECK_BUTTON(explicit_toggle_));
    std::size_t skipped = 0;
    for (const auto index : selected()) {
        auto& document = documents_[index];
        const auto current = value(document, "title");
        // Without a title there is nothing for the badge to follow.
        if (on && with_explicit(current).empty()) { ++skipped; continue; }
        stage_tag(document, "title", on ? with_explicit(current) : without_explicit(current));
    }
    // Titles differ between files, so refresh the entry and toggle rather than writing one text into all of them.
    refresh_editor();
    if (skipped) set_status(std::to_string(skipped) + (skipped == 1 ? " file has" : " files have") + " no title yet, so no explicit mark was added.");
}

void Window::edit_field(const std::string& key) {
    const std::string text = gtk_editable_get_text(GTK_EDITABLE(fields_.at(key)));
    const auto indices = selected();
    // Title and track number belong to one recording, so only album-wide tags are remembered.
    if (!indices.empty() && key != "title" && key != "track") { carry_[key] = text; ++carry_generation_; }
    for (const auto index : indices) stage_tag(documents_[index], key, text);
    if (key == "title") refresh_explicit();
    refresh_dirty();
}

void Window::stage_tag(Document& document, const std::string& key, const std::string& text) {
    const auto original = document.track.tags.find(key);
    if (text == (original == document.track.tags.end() ? "" : original->second)) document.pending.erase(key);
    else document.pending[key] = text;
}

std::string Window::apply_carry() {
    const auto indices = selected();
    if (indices.size() != 1 || (carry_.empty() && !carry_cover_) || !gtk_check_button_get_active(GTK_CHECK_BUTTON(carry_toggle_))) return {};
    auto& document = documents_[indices.front()];
    if (document.carried == carry_generation_) return {};
    document.carried = carry_generation_;
    if (document.dirty()) return {};
    std::string names;
    for (const auto& [key, caption] : tag_fields) {
        const auto found = carry_.find(key);
        if (found == carry_.end()) continue;
        const auto original = document.track.tags.find(key);
        if (found->second == (original == document.track.tags.end() ? "" : original->second)) continue;
        document.pending[key] = found->second;
        names += (names.empty() ? "" : ", ") + std::string(caption);
    }
    if (carry_cover_ && carry_cover_hash_ != document.track.cover_hash) {
        document.cover_edit = carry_cover_;
        document.cover_edit_hash = carry_cover_hash_;
        names += (names.empty() ? "" : ", ") + std::string(carry_cover_->image ? "Cover" : "Cover removal");
    }
    if (names.empty()) return {};
    return "Copied " + names + " from your last edit. Revert selected edits to undo.";
}

void Window::clear_field(const std::string& key) {
    auto* entry = fields_.at(key);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    edit_field(key);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Tag will be removed");
}

std::size_t Window::dirty_count() const {
    return static_cast<std::size_t>(std::count_if(documents_.begin(), documents_.end(), [](const auto& document) { return document.dirty(); }));
}

void Window::refresh_dirty_indicators() {
    for (auto* row = gtk_widget_get_first_child(files_); row; row = gtk_widget_get_next_sibling(row)) {
        if (!GTK_IS_LIST_BOX_ROW(row)) continue;
        const auto index = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "document-index")) - 1;
        auto* indicator = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(row), "dirty-indicator"));
        if (index >= documents_.size() || !indicator) continue;
        const auto& document = documents_[index];
        const bool dirty = document.dirty();
        gtk_label_set_text(GTK_LABEL(indicator), dirty ? "●" : duration(document.track.duration_seconds).c_str());
        gtk_widget_remove_css_class(indicator, dirty ? "duration" : "dirty");
        gtk_widget_add_css_class(indicator, dirty ? "dirty" : "duration");
    }
}

void Window::refresh_dirty() {
    refresh_dirty_indicators();
    const auto count = dirty_count();
    gtk_widget_set_sensitive(save_, count != 0);
    gtk_window_set_title(GTK_WINDOW(window_), count ? "MetaDots • Unsaved changes" : "MetaDots");
    if (count) set_status(count_of(count, "file") + (count == 1 ? " has" : " have") + " unsaved edits. Ctrl+S to review and save.");
}

GtkWidget* Window::begin_modal(const std::string& title) {
    dismiss_modal();
    // A modal still fading out is replaced in place; the new one picks up its progress so nothing dips.
    if (shade_) {
        gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_), shade_);
        shade_ = scale_ = nullptr;
    }
    modal_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(modal_, "modal-shade");
    gtk_widget_set_hexpand(modal_, true);
    gtk_widget_set_vexpand(modal_, true);
    gtk_widget_set_limit_events(modal_, true);
    modal_card_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(modal_card_, "modal-card");
    gtk_widget_set_size_request(modal_card_, 440, -1);
    auto* heading = label(title, "modal-title");
    gtk_box_append(GTK_BOX(modal_card_), heading);
    modal_feedback_ = label("", "modal-feedback");
    gtk_label_set_wrap(GTK_LABEL(modal_feedback_), true);
    gtk_label_set_max_width_chars(GTK_LABEL(modal_feedback_), 54);
    gtk_widget_set_visible(modal_feedback_, false);
    gtk_box_append(GTK_BOX(modal_card_), modal_feedback_);
    modal_viewport_ = scroll(modal_card_);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(modal_viewport_), true);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(modal_viewport_), true);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(modal_viewport_), 650);
    scale_ = metadots_scale_bin_new(modal_viewport_);
    gtk_widget_set_hexpand(scale_, true);
    gtk_widget_set_vexpand(scale_, true);
    gtk_widget_set_halign(scale_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(scale_, GTK_ALIGN_CENTER);
    margins(scale_, 24);
    gtk_box_append(GTK_BOX(modal_), scale_);
    shade_ = modal_;
    set_modal_progress(modal_progress_);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), modal_);
    set_content_inert(true);
    animate_modal(1);
    auto* outside = gtk_gesture_click_new();
    g_signal_connect(outside, "released", G_CALLBACK((+[](GtkGestureClick*, int, double x, double y, gpointer data) {
        auto& self = *static_cast<Window*>(data);
        if (!self.modal_viewport_) return;
        const graphene_point_t point{static_cast<float>(x), static_cast<float>(y)};
        graphene_point_t local{};
        if (gtk_widget_compute_point(self.modal_, self.modal_viewport_, &point, &local) &&
            !gtk_widget_contains(self.modal_viewport_, local.x, local.y)) self.dismiss_modal();
    })), this);
    gtk_widget_add_controller(modal_, GTK_EVENT_CONTROLLER(outside));
    return modal_card_;
}

void Window::show_modal(const std::string& title, const std::string& message,
                        const std::string& confirm, std::function<void()> action) {
    auto* card = begin_modal(title);
    auto* body = label(message, "muted");
    gtk_label_set_wrap(GTK_LABEL(body), true);
    gtk_label_set_max_width_chars(GTK_LABEL(body), 54);
    gtk_box_append(GTK_BOX(card), body);
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_widget_set_margin_top(actions, 6);
    gtk_box_append(GTK_BOX(actions), button("Cancel", nullptr, [this] { dismiss_modal(); }));
    modal_action_ = std::move(action);
    auto* accept = button(confirm.c_str(), nullptr, [this] {
        auto accept_action = std::move(modal_action_);
        dismiss_modal();
        if (accept_action) accept_action();
    });
    gtk_widget_add_css_class(accept, "suggested-action");
    gtk_box_append(GTK_BOX(actions), accept);
    gtk_box_append(GTK_BOX(card), actions);
    gtk_widget_grab_focus(accept);
}

void Window::dismiss_modal() {
    if (!modal_) return;
    // The widgets stay on screen while they fade, but they no longer take clicks.
    gtk_widget_set_can_target(modal_, false);
    modal_ = modal_card_ = modal_viewport_ = modal_feedback_ = browser_entry_ = browser_list_ = nullptr;
    modal_action_ = {};
    set_content_inert(false);
    gtk_widget_grab_focus(search_);
    animate_modal(0);
}

// Insensitive widgets restyle as disabled, which would pop visibly behind a fading shade.
void Window::set_content_inert(bool inert) {
    gtk_widget_set_can_target(content_, !inert);
    gtk_widget_set_can_focus(content_, !inert);
}

void Window::set_modal_progress(double progress) {
    modal_progress_ = progress;
    if (shade_) gtk_widget_set_opacity(shade_, progress);
    if (scale_) metadots_scale_bin_set_scale(scale_, .98 + .02 * progress);
    metadots_blur_bin_set_blur(blur_, progress);
}

void Window::finish_fade() {
    if (modal_progress_ > 0 || modal_ || !shade_) return;
    gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_), shade_);
    shade_ = scale_ = nullptr;
}

// Same motion as Solitare's modals: 120 ms, ease-out cubic, fade with a 0.98 to 1 scale.
void Window::animate_modal(double target) {
    gboolean animations = true;
    g_object_get(gtk_settings_get_for_display(display_), "gtk-enable-animations", &animations, nullptr);
    motion_target_ = target;
    motion_from_ = modal_progress_;
    motion_start_ = 0;
    if (motion_from_ == target || !animations || !gtk_widget_get_mapped(window_)) {
        if (motion_tick_) { gtk_widget_remove_tick_callback(window_, motion_tick_); motion_tick_ = 0; }
        set_modal_progress(target);
        finish_fade();
        return;
    }
    if (motion_tick_) return;
    motion_tick_ = gtk_widget_add_tick_callback(window_, +[](GtkWidget*, GdkFrameClock* clock, gpointer data) -> gboolean {
        auto& self = *static_cast<Window*>(data);
        const gint64 now = gdk_frame_clock_get_frame_time(clock);
        if (!self.motion_start_) self.motion_start_ = now;
        const double t = std::min(1.0, static_cast<double>(now - self.motion_start_) / 120000.0);
        const double eased = 1 - std::pow(1 - t, 3);
        self.set_modal_progress(self.motion_from_ + (self.motion_target_ - self.motion_from_) * eased);
        if (t < 1) return G_SOURCE_CONTINUE;
        self.motion_tick_ = 0;
        self.finish_fade();
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

void Window::request_open() {
    if (settings_.use_native_picker) open_browser();
    else open_menu();
}

// The system chooser picks either files or a folder, never both, so the choice comes first.
void Window::open_menu() {
    auto* card = begin_modal("Open audio files");
    auto* body = label("The system file picker opens next.", "muted");
    gtk_box_append(GTK_BOX(card), body);
    const std::array<std::pair<const char*, Pick>, 3> choices{{
        {"Choose audio files…", Pick::Files}, {"Choose a folder…", Pick::Folder},
        {"Choose a folder with its subfolders…", Pick::Subfolders}}};
    GtkWidget* first = nullptr;
    for (const auto& [text, pick] : choices) {
        auto* choice = button(text, nullptr, [this, pick = pick] { dismiss_modal(); system_pick(pick); });
        if (!first) first = choice;
        gtk_box_append(GTK_BOX(card), choice);
    }
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(actions), button("Cancel", nullptr, [this] { dismiss_modal(); }));
    gtk_box_append(GTK_BOX(card), actions);
    gtk_widget_grab_focus(first);
}

void Window::system_pick(Pick pick) {
    struct Request { Window* owner; std::weak_ptr<bool> alive; Pick pick; };
    if (!picker_cancel_) picker_cancel_ = g_cancellable_new();
    std::unique_ptr<GtkFileDialog, decltype(&g_object_unref)> dialog(gtk_file_dialog_new(), g_object_unref);
    const bool image = pick == Pick::Image;
    gtk_file_dialog_set_title(dialog.get(), image ? "Choose cover image" : pick == Pick::Files ? "Open audio files" : "Open folder");
    if (pick == Pick::Files || image) {
        auto* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, image ? "JPEG and PNG images" : "Audio files");
        if (image) { gtk_file_filter_add_mime_type(filter, "image/jpeg"); gtk_file_filter_add_mime_type(filter, "image/png"); }
        else gtk_file_filter_add_mime_type(filter, "audio/*");
        auto* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        g_list_store_append(filters, filter);
        gtk_file_dialog_set_filters(dialog.get(), G_LIST_MODEL(filters));
        g_object_unref(filter);
        g_object_unref(filters);
    }
    // Like the in-window browser: covers start beside the selected file, everything else where the last pick ended.
    auto start = browser_path_;
    const auto chosen = selected();
    if (image && !chosen.empty()) start = documents_[chosen.front()].track.path.parent_path();
    std::error_code ignored;
    if (start.empty() || !std::filesystem::is_directory(start, ignored)) start = g_get_home_dir();
    std::unique_ptr<GFile, decltype(&g_object_unref)> folder(g_file_new_for_path(start.c_str()), g_object_unref);
    gtk_file_dialog_set_initial_folder(dialog.get(), folder.get());
    auto* pending = new Request{this, alive_, pick};
    const auto finish = [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Request> request(static_cast<Request*>(data));
        auto* chooser = GTK_FILE_DIALOG(source);
        GError* error = nullptr;
        std::vector<std::filesystem::path> paths;
        const auto add = [&](GFile* file) {
            if (!file) return;
            if (auto* path = g_file_get_path(file)) { paths.emplace_back(path); g_free(path); }
        };
        switch (request->pick) {
            case Pick::Files: {
                std::unique_ptr<GListModel, decltype(&g_object_unref)> files(gtk_file_dialog_open_multiple_finish(chooser, result, &error), g_object_unref);
                if (files) for (guint index = 0; index < g_list_model_get_n_items(files.get()); ++index) {
                    std::unique_ptr<GFile, decltype(&g_object_unref)> file(static_cast<GFile*>(g_list_model_get_item(files.get(), index)), g_object_unref);
                    add(file.get());
                }
                break;
            }
            case Pick::Image: {
                std::unique_ptr<GFile, decltype(&g_object_unref)> file(gtk_file_dialog_open_finish(chooser, result, &error), g_object_unref);
                add(file.get());
                break;
            }
            default: {
                std::unique_ptr<GFile, decltype(&g_object_unref)> file(gtk_file_dialog_select_folder_finish(chooser, result, &error), g_object_unref);
                add(file.get());
            }
        }
        std::string message;
        // Closing the chooser without picking anything is not an error.
        if (error && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
            !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) message = error->message ? error->message : "The file picker failed.";
        g_clear_error(&error);
        if (!request->alive.lock() || request->owner->closing_) return;
        auto& self = *request->owner;
        if (!message.empty()) { self.show_error(message.c_str()); return; }
        if (paths.empty()) return;
        self.guarded([&] {
            if (request->pick == Pick::Image) self.set_cover_from_file(paths.front());
            else {
                if (request->pick != Pick::Files && std::filesystem::is_directory(paths.front())) self.browser_path_ = paths.front();
                self.open_paths(paths, request->pick == Pick::Subfolders);
            }
        });
    };
    switch (pick) {
        case Pick::Files: gtk_file_dialog_open_multiple(dialog.get(), GTK_WINDOW(window_), picker_cancel_, finish, pending); break;
        case Pick::Image: gtk_file_dialog_open(dialog.get(), GTK_WINDOW(window_), picker_cancel_, finish, pending); break;
        default: gtk_file_dialog_select_folder(dialog.get(), GTK_WINDOW(window_), picker_cancel_, finish, pending);
    }
}

void Window::save_settings_now() {
    save_settings(settings_file_, settings_);
}

void Window::open_settings() {
    auto* card = begin_modal("Settings");
    auto* toggle = gtk_check_button_new_with_label("Use native picker");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(toggle), settings_.use_native_picker);
    connect_action(toggle, "toggled", [this, toggle] {
        settings_.use_native_picker = gtk_check_button_get_active(GTK_CHECK_BUTTON(toggle));
        save_settings_now();
    });
    gtk_box_append(GTK_BOX(card), toggle);
    auto* note = label("On: MetaDots' own file browser inside this window. Off: your system's file picker.", "muted");
    gtk_label_set_wrap(GTK_LABEL(note), true);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 54);
    gtk_box_append(GTK_BOX(card), note);
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    auto* done = button("Done", nullptr, [this] { dismiss_modal(); });
    gtk_widget_add_css_class(done, "suggested-action");
    gtk_box_append(GTK_BOX(actions), done);
    gtk_box_append(GTK_BOX(card), actions);
    gtk_widget_grab_focus(toggle);
}

void Window::request_save() {
    const auto count = dirty_count();
    if (!count) return;
    show_modal("Save metadata?", "Write your pending tag edits to " + count_of(count, "audio file") + "? Each file is saved separately; files changed by another program will be skipped.", "Save changes", [this] { save_changes(); });
}

void Window::save_changes() {
    std::size_t saved = 0, failed = 0;
    std::string errors;
    for (auto& document : documents_) {
        if (!document.dirty()) continue;
        try {
            document.track = save_tags(document.track, document.pending, document.cover_edit ? &*document.cover_edit : nullptr);
            document.pending.clear();
            document.cover_edit.reset();
            document.cover_edit_hash = 0;
            ++saved;
        } catch (const std::exception& error) {
            ++failed;
            if (errors.size() < 2000) errors += document.track.path.filename().string() + ": " + error.what() + "\n";
        }
    }
    rebuild_files();
    set_status(count_of(saved, "file") + " saved." + (failed ? " " + std::to_string(failed) + " failed; their edits are still pending." : ""));
    if (failed) show_modal("Some files were not saved", errors, "Keep editing", [] {});
    else if (close_after_save_) close_window();
    close_after_save_ = false;
}

void Window::request_revert() {
    const auto indices = selected();
    if (std::none_of(indices.begin(), indices.end(), [&](auto index) { return documents_[index].dirty(); })) return;
    show_modal("Revert selected edits?", "Discard pending metadata edits for the selected files?", "Revert edits", [this, indices] {
        for (auto index : indices) { documents_[index].pending.clear(); documents_[index].cover_edit.reset(); documents_[index].cover_edit_hash = 0; }
        rebuild_files();
        set_status("Selected edits reverted.");
    });
}

void Window::request_close() {
    if (!dirty_count()) { close_window(); return; }
    show_modal("You have unsaved edits", "Save your metadata edits before closing MetaDots?", "Save and close", [this] { close_after_save_ = true; save_changes(); });
    auto* discard = button("Discard and close", nullptr, [this] { close_window(); });
    gtk_widget_add_css_class(discard, "destructive");
    gtk_box_prepend(GTK_BOX(gtk_widget_get_last_child(modal_card_)), discard);
}

void Window::open_browser(bool images) {
    auto* card = begin_modal(images ? "Choose cover image" : "Open audio files");
    browser_images_ = images;
    gtk_widget_set_size_request(card, 560, -1);
    auto* navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(navigation), button("Up", "go-up-symbolic", [this] { browse(browser_path_.parent_path()); }));
    browser_entry_ = gtk_entry_new();
    gtk_widget_set_hexpand(browser_entry_, true);
    connect_action(browser_entry_, "activate", [this] { browse(gtk_editable_get_text(GTK_EDITABLE(browser_entry_))); });
    gtk_box_append(GTK_BOX(navigation), browser_entry_);
    gtk_box_append(GTK_BOX(card), navigation);
    browser_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(browser_list_), images ? GTK_SELECTION_SINGLE : GTK_SELECTION_MULTIPLE);
    // A single click only selects, so Ctrl+click and Shift+click can build a group; double-click opens.
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(browser_list_), false);
    auto* viewport = scroll(browser_list_);
    gtk_widget_set_size_request(viewport, -1, 260);
    gtk_box_append(GTK_BOX(card), viewport);
    gtk_box_append(GTK_BOX(card), label(images ? "Choose a JPEG or PNG image. Double-click a folder to enter it, or an image to use it."
                                               : "Ctrl + click or Shift + click to select several files. Double-click a folder to enter it.", "muted"));
    g_signal_connect(browser_list_, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
        auto& self = *static_cast<Window*>(data);
        const auto* raw = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "file-path"));
        if (raw) self.guarded([&] {
            const std::filesystem::path path(raw);
            if (std::filesystem::is_directory(path)) self.browse(path);
            else {
                if (!gtk_list_box_row_is_selected(row)) {
                    gtk_list_box_unselect_all(GTK_LIST_BOX(self.browser_list_));
                    gtk_list_box_select_row(GTK_LIST_BOX(self.browser_list_), row);
                }
                self.browser_accept();
            }
        });
    }), this);
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(actions), button("Cancel", nullptr, [this] { dismiss_modal(); }));
    if (!images) {
        gtk_box_append(GTK_BOX(actions), button("Open folder", nullptr, [this] {
            const auto path = browser_path_; dismiss_modal(); open_paths({path}, false);
        }));
        gtk_box_append(GTK_BOX(actions), button("Include subfolders", nullptr, [this] {
            const auto path = browser_path_; dismiss_modal(); open_paths({path}, true);
        }));
    }
    auto* accept = button(images ? "Use image" : "Open selected", nullptr, [this] { browser_accept(); });
    gtk_widget_add_css_class(accept, "suggested-action");
    gtk_box_append(GTK_BOX(actions), accept);
    gtk_box_append(GTK_BOX(card), actions);
    // Cover art usually sits beside the album, so start in the selected file's folder.
    const auto chosen = selected();
    if (images && !chosen.empty() && std::filesystem::is_directory(documents_[chosen.front()].track.path.parent_path()))
        browser_path_ = documents_[chosen.front()].track.path.parent_path();
    browse(browser_path_.empty() ? std::filesystem::path(g_get_home_dir()) : browser_path_);
    gtk_widget_grab_focus(browser_entry_);
}

void Window::browse(const std::filesystem::path& requested) {
    const auto path = std::filesystem::absolute(requested).lexically_normal();
    if (std::filesystem::is_regular_file(path)) {
        if (browser_images_) set_cover_from_file(path);
        else { dismiss_modal(); open_paths({path}, false); }
        return;
    }
    if (!std::filesystem::is_directory(path)) throw std::runtime_error("Folder does not exist: " + path.string());
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied)) {
        if (!entry.is_symlink() && entry.is_directory() && !entry.path().filename().string().starts_with('.')) folders.push_back(entry.path());
        if (folders.size() >= 2000) break;
    }
    std::sort(folders.begin(), folders.end());
    auto files = browser_images_ ? scan_images(path, 2000) : scan_audio(path, false, 2000);
    folders.insert(folders.end(), files.begin(), files.end());
    browser_path_ = path;
    gtk_widget_set_visible(modal_feedback_, false);
    gtk_editable_set_text(GTK_EDITABLE(browser_entry_), path.c_str());
    gtk_list_box_remove_all(GTK_LIST_BOX(browser_list_));
    for (const auto& item : folders) {
        auto* row = gtk_list_box_row_new();
        g_object_set_data_full(G_OBJECT(row), "file-path", g_strdup(item.c_str()), g_free);
        auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(std::filesystem::is_directory(item) ? "folder-symbolic" : browser_images_ ? "image-x-generic-symbolic" : "audio-x-generic-symbolic"));
        auto* name = label(item.filename().string());
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(box), name);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(GTK_LIST_BOX(browser_list_), row);
    }
}

void Window::browser_accept() {
    if (browser_images_) {
        auto* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(browser_list_));
        const auto* raw = row ? static_cast<const char*>(g_object_get_data(G_OBJECT(row), "file-path")) : nullptr;
        if (!raw) { browse(gtk_editable_get_text(GTK_EDITABLE(browser_entry_))); return; }
        const std::filesystem::path chosen(raw);
        if (std::filesystem::is_directory(chosen)) browse(chosen);
        else set_cover_from_file(chosen);
        return;
    }
    std::vector<std::filesystem::path> paths;
    std::unique_ptr<GList, decltype(&g_list_free)> rows(gtk_list_box_get_selected_rows(GTK_LIST_BOX(browser_list_)), g_list_free);
    for (auto* row = rows.get(); row; row = row->next) {
        const auto* path = static_cast<const char*>(g_object_get_data(G_OBJECT(row->data), "file-path"));
        if (path) paths.emplace_back(path);
    }
    if (paths.empty()) { browse(gtk_editable_get_text(GTK_EDITABLE(browser_entry_))); return; }
    dismiss_modal();
    open_paths(paths, false);
}

void Window::screenshot(const std::filesystem::path& path) {
    std::unique_ptr<GdkPaintable, decltype(&g_object_unref)> paintable(gtk_widget_paintable_new(window_), g_object_unref);
    auto* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable.get(), GDK_SNAPSHOT(snapshot), gtk_widget_get_width(window_), gtk_widget_get_height(window_));
    std::unique_ptr<GskRenderNode, decltype(&gsk_render_node_unref)> node(gtk_snapshot_free_to_node(snapshot), gsk_render_node_unref);
    if (!node) throw std::runtime_error("Window has no rendered frame to capture");
    auto* renderer = gtk_native_get_renderer(GTK_NATIVE(window_));
    if (!renderer) throw std::runtime_error("Window renderer is unavailable");
    std::unique_ptr<GdkTexture, decltype(&g_object_unref)> texture(gsk_renderer_render_texture(renderer, node.get(), nullptr), g_object_unref);
    if (!texture || !gdk_texture_save_to_png(texture.get(), path.c_str())) throw std::runtime_error("Preview could not be saved");
}

bool Window::self_test(const std::filesystem::path& screenshots, bool capture_modal) {
    if (capture_modal) {
        screenshot(screenshots / "modal.png");
        dismiss_modal();
        return true;
    }
    const auto require = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
    require(gtk_window_get_resizable(GTK_WINDOW(window_)), "Window is not resizable");
    if (!screenshots.empty()) { std::filesystem::create_directories(screenshots); screenshot(screenshots / "main.png"); }
    const auto original_theme = theme_;
    auto edited = original_theme;
    edited.colors["accent"] = "#ac8fdb";
    edited.source_kind.clear(); edited.source_path.clear();
    save_theme(config_, edited);
    refresh_theme();
    require(theme_.colors.at("accent") == "#ac8fdb", "Live theme reload failed");
    save_theme(config_, original_theme);
    refresh_theme();
    show_modal("Embedded modal", "All dialogs stay inside MetaDots. The content behind this card is blurred.", "Close", [] {});
    require(modal_ && !GTK_IS_WINDOW(modal_), "Modal created an external window");
    require(!gtk_widget_get_can_target(content_) && !gtk_widget_get_can_focus(content_), "Background remains interactive");
    require(g_list_model_get_n_items(gtk_window_get_toplevels()) == 1, "More than one top-level window");
    auto* modal_actions = gtk_widget_get_last_child(modal_card_);
    g_signal_emit_by_name(gtk_widget_get_last_child(modal_actions), "clicked");
    require(!modal_, "Modal action did not dismiss its own widget");
    require(gtk_widget_get_can_target(content_) && gtk_widget_get_can_focus(content_), "Dismiss did not restore content");
    // A closing modal fades in place, and a modal opened meanwhile takes over at the same progress.
    show_modal("Handoff", "Replaces a fading modal.", "Close", [] {});
    set_modal_progress(.6);
    dismiss_modal();
    require(!modal_ && shade_ && gtk_widget_get_opacity(shade_) == .6, "Closing modal lost its progress");
    show_modal("Handoff", "Replaces a fading modal.", "Close", [] {});
    require(modal_ == shade_ && modal_progress_ == .6 && gtk_widget_get_opacity(shade_) == .6, "Handoff did not keep the modal progress");
    set_modal_progress(0);
    dismiss_modal();
    finish_fade();
    require(!shade_ && !modal_, "Faded modal stayed on screen");
    if (!documents_.empty()) {
        gtk_list_box_select_all(GTK_LIST_BOX(files_));
        const auto count = selected().size();
        gtk_editable_set_text(GTK_EDITABLE(fields_.at("title")), "MetaDots staged edit");
        require(dirty_count() == count, "Multi-file staging failed");
        for (auto& document : documents_) document.pending.clear();
        refresh_editor();
        require(dirty_count() == 0, "Pending edits were not cleared");
    }
    open_browser();
    require(browser_list_ && modal_, "In-window browser failed");
    require(!gtk_list_box_get_activate_on_single_click(GTK_LIST_BOX(files_)), "Library would ignore Ctrl+click");
    require(!gtk_list_box_get_activate_on_single_click(GTK_LIST_BOX(browser_list_)), "Browser would open a file on Ctrl+click");
    guarded([&] { browse(config_ / "missing-directory"); });
    require(gtk_widget_get_visible(modal_feedback_), "Browser error was not visible inside the modal");
    dismiss_modal();
    auto original_documents = std::move(documents_);
    Document first, second;
    first.track.path = "mixed-first.wav";
    first.track.tags["title"] = "First title";
    second.track.path = "mixed-second.wav";
    second.track.tags["title"] = "Second title";
    documents_ = {std::move(first), std::move(second)};
    rebuild_files();
    gtk_list_box_select_all(GTK_LIST_BOX(files_));
    auto* title_entry = fields_.at("title");
    require(std::string(gtk_editable_get_text(GTK_EDITABLE(title_entry))).empty(), "Mixed titles were not represented by an empty editor");
    g_signal_emit_by_name(title_entry, "icon-release", GTK_ENTRY_ICON_SECONDARY);
    require(dirty_count() == 2 && documents_[0].pending.at("title").empty() && documents_[1].pending.at("title").empty(), "Explicit clear did not stage removal of mixed titles");
    auto* first_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(files_), 0);
    auto* indicator = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(first_row), "dirty-indicator"));
    require(indicator && std::string(gtk_label_get_text(GTK_LABEL(indicator))) == "●", "Dirty row indicator was not updated after editing");
    for (auto& document : documents_) document.pending.clear();
    carry_.clear();
    // The explicit mark is a per-file suffix, so toggling a mixed selection keeps every title.
    refresh_editor();
    auto* explicit_box = GTK_CHECK_BUTTON(explicit_toggle_);
    require(!gtk_check_button_get_active(explicit_box) && !gtk_check_button_get_inconsistent(explicit_box), "Explicit toggle starts checked");
    gtk_check_button_set_active(explicit_box, true);
    const std::string mark(explicit_suffix);
    require(documents_[0].pending.at("title") == "First title" + mark && documents_[1].pending.at("title") == "Second title" + mark, "Explicit toggle did not keep each file's own title");
    require(gtk_check_button_get_active(explicit_box) && !gtk_check_button_get_inconsistent(explicit_box), "Explicit toggle did not stay checked");
    gtk_check_button_set_active(explicit_box, false);
    require(dirty_count() == 0, "Explicit toggle off did not drop the pending titles");
    documents_[0].track.tags["title"] = "Already" + mark;
    refresh_editor();
    require(gtk_check_button_get_inconsistent(explicit_box), "Mixed explicit state was not shown");
    gtk_check_button_set_active(explicit_box, true);
    require(documents_[1].pending.at("title") == "Second title" + mark && !documents_[0].pending.count("title"), "Explicit toggle doubled the mark");
    gtk_check_button_set_active(explicit_box, false);
    require(documents_[0].pending.at("title") == "Already" && documents_[1].pending.count("title") == 0, "Explicit toggle off did not strip the mark");
    for (auto& document : documents_) document.pending.clear();
    documents_[0].track.tags["title"] = "First title";
    gtk_list_box_unselect_all(GTK_LIST_BOX(files_));
    gtk_list_box_select_row(GTK_LIST_BOX(files_), gtk_list_box_get_row_at_index(GTK_LIST_BOX(files_), 0));
    gtk_editable_set_text(GTK_EDITABLE(fields_.at("title")), ("Typed by hand" + mark).c_str());
    require(gtk_check_button_get_active(explicit_box), "Typing the mark did not check the toggle");
    gtk_editable_set_text(GTK_EDITABLE(fields_.at("title")), "");
    gtk_check_button_set_active(explicit_box, true);
    require(!documents_[0].pending.count("title") || documents_[0].pending.at("title").empty(), "Explicit toggle invented a title");
    require(!gtk_check_button_get_active(explicit_box), "Explicit toggle stayed on for an empty title");
    for (auto& document : documents_) document.pending.clear();
    refresh_editor();
    gtk_list_box_unselect_all(GTK_LIST_BOX(files_));
    gtk_list_box_select_row(GTK_LIST_BOX(files_), gtk_list_box_get_row_at_index(GTK_LIST_BOX(files_), 0));
    gtk_editable_set_text(GTK_EDITABLE(fields_.at("artist")), "Carried artist");
    gtk_editable_set_text(GTK_EDITABLE(fields_.at("title")), "Stays here");
    gtk_list_box_unselect_all(GTK_LIST_BOX(files_));
    gtk_list_box_select_row(GTK_LIST_BOX(files_), gtk_list_box_get_row_at_index(GTK_LIST_BOX(files_), 1));
    require(documents_[1].pending.count("artist") && documents_[1].pending.at("artist") == "Carried artist", "Last edit was not carried to the next file");
    require(!documents_[1].pending.count("title"), "Title must not be carried");
    require(std::string(gtk_editable_get_text(GTK_EDITABLE(fields_.at("artist")))) == "Carried artist", "Carried value is not shown in the editor");
    // Covers are staged like other edits, shown in the editor, and carried to the next file.
    const auto make_png = [](guint8 red) {
        const guint8 pixel[4] = {red, 40, 90, 255};
        std::unique_ptr<GBytes, decltype(&g_bytes_unref)> raw(g_bytes_new(pixel, 4), g_bytes_unref);
        std::unique_ptr<GdkTexture, decltype(&g_object_unref)> texture(gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, raw.get(), 4), g_object_unref);
        std::unique_ptr<GBytes, decltype(&g_bytes_unref)> png(gdk_texture_save_to_png_bytes(texture.get()), g_bytes_unref);
        Cover cover;
        cover.mime = "image/png";
        const auto* bytes = static_cast<const guint8*>(g_bytes_get_data(png.get(), nullptr));
        cover.data.assign(bytes, bytes + g_bytes_get_size(png.get()));
        return cover;
    };
    const auto select_row = [&](int index) {
        gtk_list_box_unselect_all(GTK_LIST_BOX(files_));
        gtk_list_box_select_row(GTK_LIST_BOX(files_), gtk_list_box_get_row_at_index(GTK_LIST_BOX(files_), index));
    };
    const auto reset_covers = [&] {
        for (auto& document : documents_) { document.pending.clear(); document.carried = 0; document.cover_edit.reset(); document.cover_edit_hash = 0; }
        carry_.clear();
        carry_cover_.reset();
    };
    reset_covers();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(carry_toggle_), true);
    select_row(0);
    require(std::string(gtk_stack_get_visible_child_name(GTK_STACK(cover_stack_))) == "note" && !gtk_widget_get_sensitive(cover_remove_), "A file without a cover was not shown as empty");
    stage_cover(CoverChange{std::make_shared<const Cover>(make_png(200))});
    require(documents_[0].cover_edit && documents_[0].dirty() && dirty_count() == 1, "Staging a cover did not mark the file as edited");
    require(std::string(gtk_stack_get_visible_child_name(GTK_STACK(cover_stack_))) == "art" && gtk_widget_get_sensitive(cover_remove_), "A staged cover was not previewed");
    select_row(1);
    require(documents_[1].cover_edit && documents_[1].cover_edit_hash == documents_[0].cover_edit_hash, "Cover was not carried to the next file");
    require(std::string(gtk_stack_get_visible_child_name(GTK_STACK(cover_stack_))) == "art", "Carried cover is not shown in the editor");
    gtk_list_box_select_all(GTK_LIST_BOX(files_));
    require(std::string(gtk_label_get_text(GTK_LABEL(cover_note_))) == "No cover" || std::string(gtk_label_get_text(GTK_LABEL(cover_note_))) == "Mixed covers" || documents_[0].cover_hash() == documents_[1].cover_hash(), "Mixed covers were not recognised");
    reset_covers();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(carry_toggle_), false);
    select_row(0);
    stage_cover(CoverChange{std::make_shared<const Cover>(make_png(120))});
    select_row(1);
    require(!documents_[1].cover_edit, "Cover was carried while the toggle was off");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(carry_toggle_), true);
    reset_covers();
    documents_[1].track.cover_hash = 7;
    select_row(0);
    remove_cover();
    require(!documents_[0].dirty(), "Removing a missing cover marked a file as edited");
    select_row(1);
    require(documents_[1].cover_edit && !documents_[1].cover_edit->image && documents_[1].cover_hash() == 0, "Cover removal was not carried to the next file");
    reset_covers();
    documents_[1].track.cover_hash = 0;
    // Images that only look like JPEG or PNG are rejected before they reach a file.
    const auto scratch = std::filesystem::temp_directory_path() / ("metadots-cover-ui-" + std::to_string(::getpid()));
    std::filesystem::create_directories(scratch);
    {
        const auto png = make_png(30);
        std::ofstream(scratch / "good.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data.data()), static_cast<std::streamsize>(png.data.size()));
        std::ofstream(scratch / "broken.png", std::ios::binary) << "\x89PNG\r\n\x1a\n not really an image";
        std::ofstream(scratch / "notes.txt") << "text";
        std::filesystem::create_directories(scratch / "album");
    }
    select_row(0);
    bool rejected = false;
    try { set_cover_from_file(scratch / "broken.png"); } catch (const std::exception&) { rejected = true; }
    require(rejected && !documents_[0].cover_edit, "A corrupt image was accepted as a cover");
    open_browser(true);
    require(modal_ && browser_images_ && gtk_list_box_get_selection_mode(GTK_LIST_BOX(browser_list_)) == GTK_SELECTION_SINGLE, "Cover browser did not open in image mode");
    browse(scratch);
    std::size_t rows = 0;
    while (gtk_list_box_get_row_at_index(GTK_LIST_BOX(browser_list_), static_cast<int>(rows))) ++rows;
    require(rows == 3, "Cover browser should list folders and only JPEG or PNG files");
    browse(scratch / "good.png");
    require(!modal_ && documents_[0].cover_edit, "Choosing an image in the browser did not stage it");
    std::filesystem::remove_all(scratch);
    reset_covers();
    refresh_editor();
    carry_.clear();
    documents_ = std::move(original_documents);
    rebuild_files();
    if (!screenshots.empty()) show_modal("Embedded modal", "All dialogs stay inside MetaDots. The content behind this card is blurred.", "Close", [] {});
    return true;
}

int run_window(const std::filesystem::path& config, const std::vector<std::filesystem::path>& paths,
               bool test, const std::filesystem::path& screenshots) {
    struct Context {
        std::filesystem::path config, screenshots;
        std::vector<std::filesystem::path> paths;
        std::unique_ptr<Window> window;
        bool test{}, failed{}, capture_modal{};
        guint source{};
        GtkApplication* application{};
    } context{config, screenshots, paths, nullptr, test, false, false, 0, nullptr};
    // MetaDots is styled only by its own palette, so GTK must never see the user's gtk.css.
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::filesystem::path view_parent = runtime && *runtime ? std::filesystem::path(runtime) : std::filesystem::temp_directory_path();
    remove_stale_gtk_config_views(view_parent);
    const auto gtk_view = make_gtk_config_view(config_home(), view_parent);
    struct ViewCleanup {
        std::filesystem::path path;
        ~ViewCleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } view_cleanup{gtk_view};
    g_setenv("XDG_CONFIG_HOME", gtk_view.c_str(), true);
    g_setenv("GSK_RENDERER", "cairo", false);
    // The system file picker only works through the desktop portal, which GTK skips outside a sandbox.
    {
        const char* debug = std::getenv("GDK_DEBUG");
        const std::string flags = debug && *debug ? std::string(debug) + ",portals" : "portals";
        g_setenv("GDK_DEBUG", flags.c_str(), true);
    }
    auto* application = gtk_application_new("io.github.metadots", G_APPLICATION_NON_UNIQUE);
    context.application = application;
    g_set_application_name("MetaDots");
    g_signal_connect(application, "activate", G_CALLBACK(+[](GtkApplication* app, gpointer data) {
        auto& state = *static_cast<Context*>(data);
        try {
            if (!state.window) state.window = std::make_unique<Window>(app, state.config, state.paths);
            state.window->present();
            if (state.test && !state.source) state.source = g_timeout_add(300, +[](gpointer pointer) -> gboolean {
                auto& current = *static_cast<Context*>(pointer);
                try {
                    current.window->self_test(current.screenshots, current.capture_modal);
                    if (!current.screenshots.empty() && !current.capture_modal) {
                        current.capture_modal = true;
                        return G_SOURCE_CONTINUE;
                    }
                    std::cout << "UI checks passed\n";
                }
                catch (const std::exception& error) { std::cerr << error.what() << '\n'; current.failed = true; }
                current.source = 0;
                g_application_quit(G_APPLICATION(current.application));
                return G_SOURCE_REMOVE;
            }, data);
        } catch (const std::exception& error) {
            std::cerr << "MetaDots: " << error.what() << '\n';
            state.failed = true;
            g_application_quit(G_APPLICATION(app));
        }
    }), &context);
    const int result = g_application_run(G_APPLICATION(application), 0, nullptr);
    if (context.source) g_source_remove(context.source);
    context.window.reset();
    g_object_unref(application);
    return context.failed ? 1 : result;
}
}
