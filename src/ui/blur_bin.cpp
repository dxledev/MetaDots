#include "blur_bin.h"

struct MetaDotsBlurBin {
    GtkWidget parent;
    GtkWidget* child;
    gboolean blurred;
};
struct MetaDotsBlurBinClass { GtkWidgetClass parent; };
G_DEFINE_TYPE(MetaDotsBlurBin, metadots_blur_bin, GTK_TYPE_WIDGET)

static void snapshot(GtkWidget* widget, GtkSnapshot* target) {
    auto* self = reinterpret_cast<MetaDotsBlurBin*>(widget);
    if (self->blurred) gtk_snapshot_push_blur(target, 5.0);
    if (self->child) gtk_widget_snapshot_child(widget, self->child, target);
    if (self->blurred) gtk_snapshot_pop(target);
}

static void dispose(GObject* object) {
    auto* self = reinterpret_cast<MetaDotsBlurBin*>(object);
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
    self->blurred = false;
}

GtkWidget* metadots_blur_bin_new(GtkWidget* child) {
    auto* self = reinterpret_cast<MetaDotsBlurBin*>(g_object_new(metadots_blur_bin_get_type(), nullptr));
    self->child = child;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

void metadots_blur_bin_set_blur(GtkWidget* widget, bool enabled) {
    auto* self = reinterpret_cast<MetaDotsBlurBin*>(widget);
    self->blurred = enabled;
    gtk_widget_queue_draw(widget);
}
