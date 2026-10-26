#pragma once
#include <gtk/gtk.h>

GtkWidget* metadots_blur_bin_new(GtkWidget* child);
// 0 draws the child sharp, 1 fully blurred; values between cross-fade.
void metadots_blur_bin_set_blur(GtkWidget* bin, double amount);
// Call when the content changes while blurred, so the blurred copy is rendered again.
void metadots_blur_bin_invalidate(GtkWidget* bin);

// Draws its child scaled about its own center. The scale is visual only; layout and hit-testing are unchanged.
GtkWidget* metadots_scale_bin_new(GtkWidget* child);
void metadots_scale_bin_set_scale(GtkWidget* bin, double scale);
