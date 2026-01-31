#pragma once

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

#define VITE_TYPE_TAB (vite_tab_get_type())
G_DECLARE_FINAL_TYPE(ViteTab, vite_tab, VITE, TAB, GtkBox)

GtkWidget *vite_tab_new(const char *title);
const char *vite_tab_get_title(ViteTab *self);
void vite_tab_set_title(ViteTab *self, const char *title);
void vite_tab_set_active (ViteTab *self, gboolean active);
void vite_tab_set_tab_bar (ViteTab *self, gpointer tab_bar);
void vite_tab_set_separator_visible (ViteTab *self, gboolean visible);
gboolean vite_tab_is_active (ViteTab *self);
void vite_tab_set_anim_offset_x (ViteTab *self, double offset);
double vite_tab_get_anim_offset_x (ViteTab *self);
gboolean vite_tab_is_hovered (ViteTab *self);
void vite_tab_set_modified(ViteTab *self, gboolean modified);

void vite_tab_set_last_focused_child(ViteTab *self, GtkWidget *child);
GtkWidget *vite_tab_get_last_focused_child(ViteTab *self);

void vite_tab_set_loading(ViteTab *self, gboolean loading);
void vite_tab_set_progress(ViteTab *self, double progress);
gboolean vite_tab_is_loading(ViteTab *self);
void vite_tab_set_cancellable(ViteTab *self, GCancellable *cancellable);
GCancellable *vite_tab_get_cancellable(ViteTab *self);
void vite_tab_cancel_load(ViteTab *self);
void vite_tab_set_close_when_done(ViteTab *self, gboolean close);
gboolean vite_tab_get_close_when_done(ViteTab *self);

typedef enum {
    VITE_OP_NONE,
    VITE_OP_LOADING,
    VITE_OP_SAVING
} ViteTabOperationType;

void vite_tab_set_operation_type(ViteTab *self, ViteTabOperationType type);
ViteTabOperationType vite_tab_get_operation_type(ViteTab *self);

void vite_tab_set_active_dialog(ViteTab *self, AdwAlertDialog *dialog);
void vite_tab_close_active_dialog(ViteTab *self);
void vite_tab_restore_original_title(ViteTab *self);

G_END_DECLS
