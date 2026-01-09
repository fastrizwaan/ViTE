/*
 * fading-label.h - Label widget with fading edges when text overflows
 * Based on libadwaita's AdwFadingLabel
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VITE_TYPE_FADING_LABEL (vite_fading_label_get_type())
G_DECLARE_FINAL_TYPE(ViteFadingLabel, vite_fading_label, VITE, FADING_LABEL, GtkWidget)

GtkWidget   *vite_fading_label_new       (const char *label);
const char  *vite_fading_label_get_label (ViteFadingLabel *self);
void         vite_fading_label_set_label (ViteFadingLabel *self,
                                          const char      *label);
float        vite_fading_label_get_align (ViteFadingLabel *self);
void         vite_fading_label_set_align (ViteFadingLabel *self,
                                          float            align);

G_END_DECLS
