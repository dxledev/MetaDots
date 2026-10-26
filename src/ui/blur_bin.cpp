#include "blur_bin.h"

struct MetaDotsBlurBin {
    GtkWidget parent;
    GtkWidget* child;
    double amount;
    GdkTexture* frozen;
};
struct MetaDotsBlurBinClass { GtkWidgetClass parent; };
G_DEFINE_TYPE(MetaDotsBlurBin, metadots_blur_bin, GTK_TYPE_WIDGET)

constexpr double blur_radius = 5.0;

// Blurring the whole window every frame is too slow for the cairo renderer, so the blurred look is
// rendered once into a texture and cross-faded over the live content.
static void freeze(MetaDotsBlurBin* self) {
    auto* widget = GTK_WIDGET(self);
    auto* renderer = gtk_native_get_renderer(gtk_widget_get_native(widget));
    const int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
    if (!renderer || !self->child || width <= 0 || height <= 0) return;
    auto* recorder = gtk_snapshot_new();
    gtk_snapshot_push_blur(recorder, blur_radius);
    gtk_widget_snapshot_child(widget, self->child, recorder);
    gtk_snapshot_pop(recorder);
    auto* node = gtk_snapshot_free_to_node(recorder);
    if (!node) return;
    const graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, static_cast<float>(width), static_cast<float>(height));
    self->frozen = gsk_renderer_render_texture(renderer, node, &bounds);
    gsk_render_node_unref(node);
}

static void snapshot(GtkWidget* widget, GtkSnapshot* target) {
    auto* self = reinterpret_cast<MetaDotsBlurBin*>(widget);
    if (self->child) gtk_widget_snapshot_child(widget, self->child, target);
    if (self->amount <= 0) return;
    if (self->frozen && (gdk_texture_get_width(self->frozen) != gtk_widget_get_width(widget) ||
                         gdk_texture_get_height(self->frozen) != gtk_widget_get_height(widget))) g_clear_object(&self->frozen);
    if (!self->frozen) freeze(self);
    if (!self->frozen) return;
    const graphene_rect_t rect = GRAPHENE_RECT_INIT(0, 0, static_cast<float>(gtk_widget_get_width(widget)), static_cast<float>(gtk_widget_get_height(widget)));
    gtk_snapshot_push_opacity(target, self->amount);
    gtk_snapshot_append_texture(target, self->frozen, &rect);
    gtk_snapshot_pop(target);
}

static void dispose(GObject* object) {
    auto* self = reinterpret_cast<MetaDotsBlurBin*>(object);
    g_clear_object(&self->frozen);
    if (self->child) {
        auto* child = self->child;
        self->child = nullptr;
        gtk_widget_unparent(child);
    }
    G_OBJECT_CLASS(metadots_blur_bin_parent_class)->dispose(object);
}

static void metadots_blur_bin_class_init(MetaDotsBlurBinClass* klass) {
    GTK_WIDGET_CLASS(klass)->snapshot = snapshot;
    G_OBJECT_CLASS(klass)->dispose = dispose;
    gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
}

static void metadots_blur_bin_init(MetaDotsBlurBin* self) {
    self->child = nullptr;
    self->amount = 0;
    self->frozen = nullptr;
}

GtkWidget* metadots_blur_bin_new(GtkWidget* child) {
    auto* self = reinterpret_cast<MetaDotsBlurBin*>(g_object_new(metadots_blur_bin_get_type(), nullptr));
    self->child = child;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

void metadots_blur_bin_set_blur(GtkWidget* widget, double amount) {
    auto* self = reinterpret_cast<MetaDotsBlurBin*>(widget);
    if (amount <= 0) g_clear_object(&self->frozen);
    if (self->amount == amount) return;
    self->amount = amount;
    gtk_widget_queue_draw(widget);
}

void metadots_blur_bin_invalidate(GtkWidget* widget) {
    auto* self = reinterpret_cast<MetaDotsBlurBin*>(widget);
    if (!self->frozen) return;
    g_clear_object(&self->frozen);
    gtk_widget_queue_draw(widget);
}

struct MetaDotsScaleBin {
    GtkWidget parent;
    GtkWidget* child;
    double scale;
};
struct MetaDotsScaleBinClass { GtkWidgetClass parent; };
G_DEFINE_TYPE(MetaDotsScaleBin, metadots_scale_bin, GTK_TYPE_WIDGET)

static void scale_snapshot(GtkWidget* widget, GtkSnapshot* target) {
    auto* self = reinterpret_cast<MetaDotsScaleBin*>(widget);
    if (!self->child) return;
    const float cx = static_cast<float>(gtk_widget_get_width(widget)) / 2;
    const float cy = static_cast<float>(gtk_widget_get_height(widget)) / 2;
    const graphene_point_t center{cx, cy}, back{-cx, -cy};
    const float factor = static_cast<float>(self->scale);
    gtk_snapshot_save(target);
    gtk_snapshot_translate(target, &center);
    gtk_snapshot_scale(target, factor, factor);
    gtk_snapshot_translate(target, &back);
    gtk_widget_snapshot_child(widget, self->child, target);
    gtk_snapshot_restore(target);
}

static void scale_dispose(GObject* object) {
    auto* self = reinterpret_cast<MetaDotsScaleBin*>(object);
    if (self->child) {
        auto* child = self->child;
        self->child = nullptr;
        gtk_widget_unparent(child);
    }
    G_OBJECT_CLASS(metadots_scale_bin_parent_class)->dispose(object);
}

static void metadots_scale_bin_class_init(MetaDotsScaleBinClass* klass) {
    GTK_WIDGET_CLASS(klass)->snapshot = scale_snapshot;
    G_OBJECT_CLASS(klass)->dispose = scale_dispose;
    gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
}

static void metadots_scale_bin_init(MetaDotsScaleBin* self) {
    self->child = nullptr;
    self->scale = 1;
}

GtkWidget* metadots_scale_bin_new(GtkWidget* child) {
    auto* self = reinterpret_cast<MetaDotsScaleBin*>(g_object_new(metadots_scale_bin_get_type(), nullptr));
    self->child = child;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

void metadots_scale_bin_set_scale(GtkWidget* widget, double scale) {
    auto* self = reinterpret_cast<MetaDotsScaleBin*>(widget);
    if (self->scale == scale) return;
    self->scale = scale;
    gtk_widget_queue_draw(widget);
}
