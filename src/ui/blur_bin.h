#pragma once
#include <gtk/gtk.h>

GtkWidget* metadots_blur_bin_new(GtkWidget* child);
void metadots_blur_bin_set_blur(GtkWidget* bin, bool enabled);
