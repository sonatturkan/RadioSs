#ifndef RADIOS_CONTENT_SCALE_H
#define RADIOS_CONTENT_SCALE_H
#include <gtk/gtk.h>
G_BEGIN_DECLS
#define RADIOS_TYPE_CONTENT_SCALE (radios_content_scale_get_type())
G_DECLARE_FINAL_TYPE(RadioSContentScale, radios_content_scale, RADIOS, CONTENT_SCALE, GtkBin)
/* A live single-child container. Allocation and input coordinates are logical
 * child pixels; rendering scales the entire hierarchy, including native-window
 * children. Pack fixed headers/footers outside this widget. */
GtkWidget *radios_content_scale_new(void);
/* Clamped to 50..150; the default is 100. Values need not be multiples of five.
 * set_percent zooms both axes alike. set_percents zooms the horizontal and the
 * vertical lengths independently (a different pair stretches or squeezes the
 * content); get_percent returns the horizontal percentage. */
void radios_content_scale_set_percent(GtkWidget *widget, guint percent);
void radios_content_scale_set_percents(GtkWidget *widget, guint horizontal, guint vertical);
guint radios_content_scale_get_percent(GtkWidget *widget);
guint radios_content_scale_get_vertical_percent(GtkWidget *widget);
/* Section content inside a scrolling viewport must retain its scaled minimum
 * height. Ordinary viewport instances stay compressible by default. */
void radios_content_scale_set_propagate_height(GtkWidget *widget, gboolean propagate);
/* A section inside a page (e.g. the DJ mixer) zooms its own 100% layout:
 * below 100% it keeps that row-wide arrangement and just draws it narrower,
 * above 100% it fits the row while it can and never squeezes the content
 * under its natural width; the surrounding viewport scrolls instead. The main
 * workspace keeps filling. */
void radios_content_scale_set_section(GtkWidget *widget, gboolean section);
G_END_DECLS
#endif
