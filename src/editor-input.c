#include "editor-internal.h"
#include <gtk/gtk.h>
#include "editor-minimap.h"
#include <string.h>
#include <math.h>

/* Forward declarations */



static void on_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data);
static void on_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data);
static void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data);
static void on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data);
static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static gboolean on_key_released(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static void on_focus_enter(GtkEventControllerFocus *controller, gpointer user_data);
static void on_focus_leave(GtkEventControllerFocus *controller, gpointer user_data);
static void on_im_commit(GtkIMContext *context, const char *str, gpointer user_data);
static void on_ctx_cut(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer user_data) { editor_widget_cut(EDITOR_WIDGET(user_data)); }
static void on_ctx_copy(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer user_data) { editor_widget_copy(EDITOR_WIDGET(user_data)); }
static void on_ctx_paste(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer user_data) { editor_widget_paste(EDITOR_WIDGET(user_data)); }
static void on_ctx_delete(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer user_data) { editor_widget_delete(EDITOR_WIDGET(user_data)); }
static void on_ctx_undo(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer user_data) { editor_widget_undo(EDITOR_WIDGET(user_data)); }
static void on_ctx_redo(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer user_data) { editor_widget_redo(EDITOR_WIDGET(user_data)); }
static void on_ctx_select_all(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer user_data) { editor_widget_select_all(EDITOR_WIDGET(user_data)); }
static void on_ctx_change_case(GSimpleAction *action G_GNUC_UNUSED, GVariant *param, gpointer user_data) { editor_widget_change_case(EDITOR_WIDGET(user_data), g_variant_get_int32(param)); }

/* Undo/Redo Progress Callback */
void
editor_widget_on_undo_redo_progress(double progress, gboolean finished, UndoInfo *info, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    
    if (finished) {
        /* Operation complete - clear task and update UI */
        self->undo_redo_task = NULL;
        
        if (info) {
            editor_widget_clear_cursors(self);
            EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0); // Directly access since we cleared
            
            if (info->has_selection) {
                primary->selection_anchor = info->selection_start;
                primary->cursor_offset = info->selection_end;
            } else {
                if (info->is_insert) {
                    /* Was insert (or redo insert) -> place at end */
                    primary->cursor_offset = info->start + info->length;
                    primary->selection_anchor = info->start + info->length;
                } else {
                    /* Was delete (or redo delete) -> place at start */
                    primary->cursor_offset = info->start;
                    primary->selection_anchor = info->start;
                }
            }
            
            /* Sync cache variables */
            self->cursor_offset = primary->cursor_offset;
            self->selection_anchor = primary->selection_anchor;
            primary->target_x = -1;
            self->target_x = -1;
            
            /* Ensure cursor is valid/clamped */
            size_t len = document_get_length(self->doc);
            if (self->cursor_offset > len) self->cursor_offset = len;
            if (self->selection_anchor > len) self->selection_anchor = len;
            primary->cursor_offset = self->cursor_offset;
            primary->selection_anchor = self->selection_anchor;
        }
        
        /* Refresh editor */
        editor_widget_reset_cursor_blink(self);
        editor_widget_update_adjustments(self, -1, -1);
        scroll_to_cursor(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));
        
        /* Emit signal to hide progress */
        g_signal_emit_by_name(self, "undo-redo-progress", 1.0, FALSE);
    } else {
        /* Progress update */
        g_signal_emit_by_name(self, "undo-redo-progress", progress, TRUE);
    }
}

void
editor_widget_on_change_case_progress(int processed, int total, gboolean finished, gpointer user_data)
{
    double progress = (total > 0) ? (double)processed / (double)total : 1.0;
    editor_widget_on_undo_redo_progress(progress, finished, NULL, user_data);
}


void
editor_input_init_controllers(EditorWidget *self)
{
    /* Motion controller for dynamic cursor */
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_motion), self);
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), self);
    g_signal_connect(motion, "leave", G_CALLBACK(on_motion), self); /* Reuse on_motion logic or handle separately */
    gtk_widget_add_controller(GTK_WIDGET(self), motion);

    GtkEventController *controller = gtk_event_controller_key_new();
    g_signal_connect(controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(controller, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), controller);

    self->im_context = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(self->im_context, GTK_WIDGET(self));
    g_signal_connect(self->im_context, "commit", G_CALLBACK(on_im_commit), self);

    GtkEventController *focus_controller = gtk_event_controller_focus_new();
    g_signal_connect(focus_controller, "enter", G_CALLBACK(on_focus_enter), self);
    g_signal_connect(focus_controller, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus_controller);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); /* Listen to all buttons */
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), self);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag));

    /* Scroll controller for mouse wheel (works even when scrollbar is hidden) */
    GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(editor_on_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self), scroll_ctrl);
}

/* Insert Mode */
gboolean
editor_widget_get_insert_mode(EditorWidget *self)
{
    g_return_val_if_fail(EDITOR_IS_WIDGET(self), TRUE);
    return self->insert_mode;
}

void
editor_widget_set_insert_mode(EditorWidget *self, gboolean insert)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));
    if (self->insert_mode != insert) {
        self->insert_mode = insert;
        g_signal_emit_by_name(self, "insert-mode-changed");
        
        /* Redraw cursor as it might change shape (block vs bar) 
           Implementation of cursor drawing needs to check this. */
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

/* Mouse Input */


static void
on_motion(GtkEventControllerMotion *controller G_GNUC_UNUSED, double x, double y G_GNUC_UNUSED, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);

    /* If dragging, don't let motion override the cursor */
    if (self->is_drag_gesture_active) return;

    double gutter_w = get_effective_gutter_width(self);

    gboolean in_gutter = (x < gutter_w && gutter_w > 0);
    gboolean in_minimap = FALSE;

    if (self->minimap_enabled) {
        int width = gtk_widget_get_width(GTK_WIDGET(self));
        double map_w = self->minimap_width;
        if (map_w > width / 2) map_w = width / 2;

        in_minimap = (x >= width - map_w);
    }

    if (self->mouse_in_gutter != in_gutter) {
        self->mouse_in_gutter = in_gutter;
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }

    if (in_gutter) {
        /* Over gutter - use default arrow */
        gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "default");
    } else if (in_minimap) {
        /* Over minimap - use hand cursor */
        gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "hand");
    } else {
        /* Over text - use I-beam */
        gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "text");
    }
}

static void right_click(GtkGestureClick *gesture G_GNUC_UNUSED,
                        int n_press G_GNUC_UNUSED,
                        double x,
                        double y,
                        gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);

    /* Determine Context State */
    gboolean has_selection = FALSE;
    if (self->cursors) {
        for (guint c = 0; c < self->cursors->len; c++) {
            EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
            size_t s = MIN(cur->cursor_offset, cur->selection_anchor);
            size_t e = MAX(cur->cursor_offset, cur->selection_anchor);
            if (s != e) {
                has_selection = TRUE;
                break;
            }
        }
    }
    
    gboolean can_undo = self->doc && document_can_undo(self->doc);
    gboolean can_redo = self->doc && document_can_redo(self->doc);
    /* For paste: System clipboard check is async, so we default to TRUE. 
       We could check internal clipboard content if we wanted. */
    // gboolean can_paste = TRUE; 

    GMenu *menu = g_menu_new();
    GSimpleActionGroup *group = g_simple_action_group_new();

    /* Section 1: Clipboard */
    GMenu *s1 = g_menu_new();
    g_menu_append(s1, "Cut", "ctx.cut");
    g_menu_append(s1, "Copy", "ctx.copy");
    g_menu_append(s1, "Paste", "ctx.paste");
    g_menu_append(s1, "Delete", "ctx.delete");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s1));
    g_object_unref(s1);

    /* Section 2: Undo/Redo */
    GMenu *s2 = g_menu_new();
    g_menu_append(s2, "Undo", "ctx.undo");
    g_menu_append(s2, "Redo", "ctx.redo");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s2));
    g_object_unref(s2);

    /* Section 3: Selection */
    GMenu *s3 = g_menu_new();
    g_menu_append(s3, "Select All", "ctx.select-all");

    /* Change Case Submenu */
    GMenu *case_menu = g_menu_new();
    g_menu_append(case_menu, "lower case", "ctx.change-case(0)");
    g_menu_append(case_menu, "UPPER CASE", "ctx.change-case(1)");
    g_menu_append(case_menu, "Title Case", "ctx.change-case(2)");
    g_menu_append(case_menu, "iNVERT cASE", "ctx.change-case(3)");
    g_menu_append_submenu(s3, "Change Case", G_MENU_MODEL(case_menu));
    g_object_unref(case_menu);

    g_menu_append_section(menu, NULL, G_MENU_MODEL(s3));
    g_object_unref(s3);

    /* Actions */
    const GActionEntry ctx_entries[] = {
        { "cut", on_ctx_cut, NULL, NULL, NULL, { 0 } },
        { "copy", on_ctx_copy, NULL, NULL, NULL, { 0 } },
        { "paste", on_ctx_paste, NULL, NULL, NULL, { 0 } },
        { "delete", on_ctx_delete, NULL, NULL, NULL, { 0 } },
        { "undo", on_ctx_undo, NULL, NULL, NULL, { 0 } },
        { "redo", on_ctx_redo, NULL, NULL, NULL, { 0 } },
        { "select-all", on_ctx_select_all, NULL, NULL, NULL, { 0 } },
        { "change-case", on_ctx_change_case, "i", NULL, NULL, { 0 } }
    };
    g_action_map_add_action_entries(G_ACTION_MAP(group), ctx_entries, G_N_ELEMENTS(ctx_entries), self);

    /* Set Enabled States */
    GAction *act;
    
    act = g_action_map_lookup_action(G_ACTION_MAP(group), "cut");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(act), has_selection);
    
    act = g_action_map_lookup_action(G_ACTION_MAP(group), "copy");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(act), has_selection);
    
    act = g_action_map_lookup_action(G_ACTION_MAP(group), "delete");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(act), has_selection);
    
    act = g_action_map_lookup_action(G_ACTION_MAP(group), "change-case");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(act), has_selection);
    
    act = g_action_map_lookup_action(G_ACTION_MAP(group), "undo");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(act), can_undo);
    
    act = g_action_map_lookup_action(G_ACTION_MAP(group), "redo");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(act), can_redo);
    
    /* Paste always enabled for now */

    GtkWidget *popover =
        gtk_popover_menu_new_from_model_full(
            G_MENU_MODEL(menu),
            GTK_POPOVER_MENU_NESTED
        );

    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    gtk_widget_set_halign(popover, GTK_ALIGN_START);
    
    gtk_widget_set_parent(popover, GTK_WIDGET(self));
    gtk_popover_set_pointing_to(GTK_POPOVER(popover),
        &(GdkRectangle){ (int)x, (int)y, 1, 1 });
    
    gtk_widget_add_css_class(popover, "editor-context-menu");

    /* Insert Action Group */
    gtk_widget_insert_action_group(popover, "ctx", G_ACTION_GROUP(group));
    gtk_popover_popup(GTK_POPOVER(popover));

    g_object_unref(menu);
    g_object_unref(group);
}

static void
calculate_minimap_drag_ratio(EditorWidget *self)
{
    double height = gtk_widget_get_height(GTK_WIDGET(self));
    double map_content_h, map_scroll_y, map_line_h;
    editor_minimap_get_params(self, height, &map_content_h, &map_scroll_y, &map_line_h);

    /* Calculate Lens Height */
    size_t vis_start, vis_end;
    editor_widget_get_visible_line_range(self, &vis_start, &vis_end);
    if (vis_end <= vis_start) vis_end = vis_start + 1;
    
    double lens_h = (double)(vis_end - vis_start) * map_line_h;
    if (lens_h < 5.0) lens_h = 5.0; /* Match drawing minimum */
    
    /* Correct Track Height: The visual track is limited by content if shorter than widget */
    double track_h = height;
    if (map_content_h < height) track_h = map_content_h;
    
    /* Available Track */
    double available_track = track_h - lens_h;
    if (available_track < 1.0) available_track = 1.0;

    /* Max Scroll */
    double max_scroll = gtk_adjustment_get_upper(self->vadjustment) - gtk_adjustment_get_page_size(self->vadjustment);
    if (max_scroll < 0) max_scroll = 0;

    /* Calculate Ratio */
    self->drag_ratio = max_scroll / available_track;
}

static void
on_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);

    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    if (button == GDK_BUTTON_SECONDARY) {
        right_click(gesture, n_press, x, y, user_data);
        return;
    }
    
    /* Folding Click Check */
    double gutter_w = get_effective_gutter_width(self);
    double fold_w = editor_widget_get_fold_gutter_width(self);
    
    /* Minimap Click Check */
    if (self->minimap_enabled) {
        int width = gtk_widget_get_width(GTK_WIDGET(self));
        double map_w = self->minimap_width;
        if (map_w > width / 2) map_w = width / 2;

        if (x >= width - map_w) {
            /* Clicked in Minimap - consume the event to prevent it from going to viewport */
            double height = gtk_widget_get_height(GTK_WIDGET(self));

            double map_content_h, map_scroll_y, map_line_h;
            editor_minimap_get_params(self, height, &map_content_h, &map_scroll_y, &map_line_h);

            /* Calculate the clicked position relative to the minimap content */
            /* y is the mouse Y coordinate relative to the widget */
            /* We want to find which line corresponds to this Y position in the minimap */
            double clicked_y_in_minimap = y;  // y is already relative to the widget

            /* Check if clicked on Lens - need to calculate lens position accounting for scroll */
            size_t vis_start, vis_end;
            editor_widget_get_visible_line_range(self, &vis_start, &vis_end);

            /* Calculate lens position in minimap coordinates */
            if (vis_end <= vis_start) vis_end = vis_start + 1;
            double lens_y = (double)vis_start * map_line_h - map_scroll_y;
            double lens_h = (double)(vis_end - vis_start) * map_line_h;

            /* Clamp lens position to visible area */
            if (lens_y < 0) {
                lens_h += lens_y;
                lens_y = 0;
            }
            double lens_bottom = lens_y + lens_h;

            /* Check if click is within the lens bounds */
            if (clicked_y_in_minimap >= lens_y && clicked_y_in_minimap <= lens_bottom) {
                /* Clicked on Lens - GRAB IT (No Scroll) */
                /* Set the minimap_active flag to enable dragging */
                self->minimap_active = TRUE;
                /* Store the initial scroll position for drag calculations */
                if (self->vadjustment) {
                    self->drag_start_scroll = gtk_adjustment_get_value(self->vadjustment);
                }
                /* Don't return here - let the drag gesture handle the drag */
            } else {
                /* Clicked elsewhere in minimap - Move lens by one viewport based on click position */
                /* Calculate current lens position using existing variables */
                double dummy_content_h, dummy_scroll_y;
                editor_minimap_get_params(self, height, &dummy_content_h, &dummy_scroll_y, &map_line_h);

                double current_lens_y = (double)vis_start * map_line_h - dummy_scroll_y;
                // double current_lens_h = (double)(vis_end - vis_start) * map_line_h;

                /* Determine if click is above or below the lens */
                if (clicked_y_in_minimap < current_lens_y) {
                    /* Clicked above lens - move up by one viewport */
                    size_t viewport_lines = (size_t)(height / self->line_height);
                    if (viewport_lines < 1) viewport_lines = 1;

                    size_t target_line = (vis_start > viewport_lines) ? (vis_start - viewport_lines) : 0;

                    /* Ensure target line is within bounds */
                    size_t total_lines;
                    if (self->wrap_lines) {
                        total_lines = get_visual_line_count(self);
                    } else {
                        total_lines = document_get_line_count(self->doc);
                    }

                    if (target_line >= total_lines) target_line = (total_lines > 0) ? total_lines - 1 : 0;

                    editor_widget_scroll_to_line(self, target_line);
                } else {
                    /* Clicked below lens - move down by one viewport */
                    size_t viewport_lines = (size_t)(height / self->line_height);
                    if (viewport_lines < 1) viewport_lines = 1;

                    size_t target_line = vis_start + viewport_lines;

                    /* Ensure target line is within bounds */
                    size_t total_lines;
                    if (self->wrap_lines) {
                        total_lines = get_visual_line_count(self);
                    } else {
                        total_lines = document_get_line_count(self->doc);
                    }

                    if (target_line >= total_lines) target_line = (total_lines > 0) ? total_lines - 1 : 0;

                    editor_widget_scroll_to_line(self, target_line);
                }

                /* After jumping, treat as if we grabbed the lens to allow immediate dragging */
                self->minimap_active = TRUE;
                calculate_minimap_drag_ratio(self);
                if (self->vadjustment) {
                     self->drag_start_scroll = gtk_adjustment_get_value(self->vadjustment);
                } else {
                     self->drag_start_scroll = 0;
                }
            }
            /* Return here to consume the event and prevent it from going to viewport */
            return;
        }
    }
    
    // fprintf(stderr, "[DEBUG] Click: x=%.2f y=%.2f gutter_w=%.2f fold_w=%.2f\n", x, y, gutter_w, fold_w);

    /* If fold gutter allows it, check click in that specific column on the right */
    /* If fold gutter allows it, check click in that specific column on the right */
    /* Check for ANY click (n_press >= 1) to consume event and prevent selection */
    if (fold_w > 0 && x >= (gutter_w - fold_w) && x < gutter_w && n_press >= 1) {
        
        /* Toggle on any click (even fast double-clicks) to ensure responsiveness */
        size_t off;
        editor_widget_get_offset_at_point(self, x, y, &off);
        size_t line = document_get_line_of_offset(self->doc, off);
        
        if (editor_widget_toggle_fold(self, line)) {
            gtk_widget_queue_draw(GTK_WIDGET(self));
        }
        
        /* ALWAYS return to prevent line selection when clicking the fold column */
        return;
    }

    gtk_widget_grab_focus(GTK_WIDGET(self));
    
    if (!self->doc) return;

    size_t off;
    editor_widget_get_offset_at_point(self, x, y, &off);
    
    editor_widget_reset_cursor_blink(self);
    
    if (n_press == 2) {
        /* Double click - select word */
        /* Always operate on the NEW primary cursor (or the single one if cleared) */
        
        gboolean alt_held = (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)) & GDK_ALT_MASK);
        
        if (!alt_held) {
             editor_widget_clear_cursors(self);
             /* Primary cursor will be moved/expanded below */
        } else {
             /* Add new cursor if not close to existing? For now just add */
             editor_widget_add_cursor(self, off);
        }
        
        EditorCursor *primary = editor_widget_get_primary_cursor(self);
        if (!primary) return; /* Should not happen */

        size_t total = document_get_length(self->doc);
        gboolean is_newline = FALSE;
        if (off < total) {
            char *ctext = document_get_text_range(self->doc, off, 1);
            if (ctext && ctext[0] == '\n') is_newline = TRUE;
            g_free(ctext);
        }
        
        if (is_newline) {
            size_t sel_start = off;
            while (sel_start > 0) {
                char *c = document_get_text_range(self->doc, sel_start - 1, 1);
                if (c && (c[0] == ' ' || c[0] == '\t')) {
                    sel_start--;
                    g_free(c);
                } else {
                    g_free(c);
                    break;
                }
            }
            size_t sel_end = off + 1;
            if (off + 1 < total) {
                size_t next_line_start, next_line_end;
                find_line_at_offset(self->doc, off + 1, &next_line_start, &next_line_end);
                sel_end = next_line_end;
            }
            primary->selection_anchor = sel_end;
            primary->cursor_offset = sel_start;
        } else {
            gboolean is_whitespace = FALSE;
            if (off < total) {
                char *ctext = document_get_text_range(self->doc, off, 1);
                if (ctext && (ctext[0] == ' ' || ctext[0] == '\t')) is_whitespace = TRUE;
                g_free(ctext);
            }
            if (is_whitespace) {
                size_t ws_start = off;
                size_t ws_end = off;
                while (ws_start > 0) {
                    char *c = document_get_text_range(self->doc, ws_start - 1, 1);
                    if (c && (c[0] == ' ' || c[0] == '\t')) ws_start--;
                    else { g_free(c); break; }
                    g_free(c);
                }
                while (ws_end < total) {
                    char *c = document_get_text_range(self->doc, ws_end, 1);
                    if (c && (c[0] == ' ' || c[0] == '\t')) ws_end++;
                    else { g_free(c); break; }
                    g_free(c);
                }
                primary->selection_anchor = ws_end;
                primary->cursor_offset = ws_start;
            } else {
                size_t word_start, word_end;
                editor_widget_find_word_boundary(self, off, &word_start, &word_end);
                primary->selection_anchor = word_end;
                primary->cursor_offset = word_start;
            }
        }
        self->alt_word_mode = TRUE;
        self->multi_click_selection = TRUE;
        self->multi_click_mode = 2;
        self->multi_click_start = MIN(primary->selection_anchor, primary->cursor_offset);
        self->multi_click_end = MAX(primary->selection_anchor, primary->cursor_offset);
        
    } else if (n_press == 3) {
        /* Triple click */
        gboolean alt_held = (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)) & GDK_ALT_MASK);
        if (!alt_held) {
             editor_widget_clear_cursors(self);
        } else {
             editor_widget_add_cursor(self, off);
        }
        EditorCursor *primary = editor_widget_get_primary_cursor(self);
        
        size_t line_start, line_end;
        find_line_at_offset(self->doc, off, &line_start, &line_end);
        primary->selection_anchor = line_end;
        primary->cursor_offset = line_start;
        self->alt_word_mode = TRUE;
        self->multi_click_selection = TRUE;
        self->multi_click_mode = 3;
        self->multi_click_start = line_start;
        self->multi_click_end = line_end;
    } else {
        /* Single click */
        self->multi_click_selection = FALSE;
        GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
        
        if (state & GDK_ALT_MASK) {
            /* Alt+Click: Add cursor (or toggle) */
            editor_widget_add_cursor(self, off);
             /* Do NOT move primary cursor here! add_cursor creates new one at off. */
             self->alt_word_mode = FALSE;
        } else {
             /* Check if clicking inside selection of ANY cursor */
             gboolean click_in_selection = FALSE;
             // EditorCursor *clicked_cursor = NULL;
             
             for (guint c = 0; c < self->cursors->len; c++) {
                 EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                 size_t sel_start = MIN(cur->cursor_offset, cur->selection_anchor);
                 size_t sel_end = MAX(cur->cursor_offset, cur->selection_anchor);
                 if (sel_start != sel_end && off >= sel_start && off < sel_end) {
                     click_in_selection = TRUE;
                     break; 
                 }
             }

             if (state & GDK_SHIFT_MASK) {
                 /* Shift+Click: collapse to single selection extending from primary anchor */
                 editor_widget_clear_cursors(self);
                 EditorCursor *primary = editor_widget_get_primary_cursor(self);
                 
                 /* Smart Pivot on Primary */
                 size_t p_start = MIN(primary->cursor_offset, primary->selection_anchor);
                 size_t p_end = MAX(primary->cursor_offset, primary->selection_anchor);
                 
                 /* Update primary to extend to 'off' */
                 if (off >= p_end) {
                     /* Extend forward: Anchor stays at start */
                     primary->selection_anchor = p_start;
                     primary->cursor_offset = off;
                 } else if (off < p_start) {
                     /* Extend backward: Anchor moves to end */
                     primary->selection_anchor = p_end;
                     primary->cursor_offset = off;
                 } else {
                     /* Clicking inside: shrinks but keeps anchor direction usually? 
                        VS Code logic: Anchor assumes the "pivot" point. 
                        If we were selecting [10, 20] (caret at 20) and click 15 -> [10, 15] (caret 15).
                        Anchor (10) stays.
                      */
                     primary->cursor_offset = off;
                 }
                 /* Sync cache */
                 self->cursor_offset = primary->cursor_offset;
                 self->selection_anchor = primary->selection_anchor;
                 
                 self->alt_word_mode = FALSE;
             } else if (click_in_selection) {
                 /* Drag start logic */
                 self->is_dragging_selection = TRUE;
                 self->drag_start_offset = off;
             } else {
                 /* Normal click: Clear all, move primary to here */
                 editor_widget_clear_cursors(self);
                 EditorCursor *primary = editor_widget_get_primary_cursor(self);
                 primary->cursor_offset = off;
                 primary->selection_anchor = off;
                 primary->target_x = -1.0; /* Will be updated by update_target_x */
                 
                 /* Sync cache to ensure drag selection uses correct anchor */
                 self->cursor_offset = primary->cursor_offset;
                 self->selection_anchor = primary->selection_anchor;
                 self->target_x = primary->target_x;
                 self->alt_word_mode = FALSE;
             }
        }
        
        if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) == 2) {
            editor_widget_paste_primary(self);
        } else if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) == 3) {
            /* Right Click - Context Menu */
            GMenu *menu = g_menu_new();
            GSimpleActionGroup *group = g_simple_action_group_new();

            /* Section 1: Clipboard */
            GMenu *s1 = g_menu_new();
            g_menu_append(s1, "Cut", "ctx.cut");
            g_menu_append(s1, "Copy", "ctx.copy");
            g_menu_append(s1, "Paste", "ctx.paste");
            g_menu_append(s1, "Delete", "ctx.delete");
            g_menu_append_section(menu, NULL, G_MENU_MODEL(s1));
            g_object_unref(s1);

            /* Section 2: Undo/Redo */
            GMenu *s2 = g_menu_new();
            g_menu_append(s2, "Undo", "ctx.undo");
            g_menu_append(s2, "Redo", "ctx.redo");
            g_menu_append_section(menu, NULL, G_MENU_MODEL(s2));
            g_object_unref(s2);

            /* Section 3: Selection */
            GMenu *s3 = g_menu_new();
            g_menu_append(s3, "Select All", "ctx.select-all");

            /* Change Case Submenu */
            GMenu *case_menu = g_menu_new();
            g_menu_append(case_menu, "lower case", "ctx.change-case(0)");
            g_menu_append(case_menu, "UPPER CASE", "ctx.change-case(1)");
            g_menu_append(case_menu, "Title Case", "ctx.change-case(2)");
            g_menu_append(case_menu, "iNVERT cASE", "ctx.change-case(3)");
            g_menu_append_submenu(s3, "Change Case", G_MENU_MODEL(case_menu));
            g_object_unref(case_menu);

            g_menu_append_section(menu, NULL, G_MENU_MODEL(s3));
            g_object_unref(s3);

            /* Actions */
            const GActionEntry ctx_entries[] = {
                { "cut", on_ctx_cut, NULL, NULL, NULL, {0} },
                { "copy", on_ctx_copy, NULL, NULL, NULL, {0} },
                { "paste", on_ctx_paste, NULL, NULL, NULL, {0} },
                { "delete", on_ctx_delete, NULL, NULL, NULL, {0} },
                { "undo", on_ctx_undo, NULL, NULL, NULL, {0} },
                { "redo", on_ctx_redo, NULL, NULL, NULL, {0} },
                { "select-all", on_ctx_select_all, NULL, NULL, NULL, {0} },
                { "change-case", on_ctx_change_case, "i", NULL, NULL, {0} }
            };
            g_action_map_add_action_entries(G_ACTION_MAP(group), ctx_entries, G_N_ELEMENTS(ctx_entries), self);

            GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
            gtk_widget_set_parent(popover, GTK_WIDGET(self));
            gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

            GdkRectangle rect = { (int)x, (int)y, 1, 1 };
            gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

            gtk_widget_insert_action_group(popover, "ctx", G_ACTION_GROUP(group));
            gtk_popover_popup(GTK_POPOVER(popover));

            g_object_unref(menu);
            g_object_unref(group);
        }
    }
    
    update_target_x(self);
    editor_widget_update_im_cursor_location(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));

    size_t line, col;
    editor_widget_get_cursor_position(self, &line, &col);
    g_signal_emit_by_name(self, "cursor-moved", (guint)line, (guint)col);
}

static void
on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);

    /* Claim the sequence to prevent ScrolledWindow from intercepting it for scrolling */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    if (!self->doc) return;

    /* Block drag/selection if starting in fold gutter */
    double gutter_w = get_effective_gutter_width(self);
    double fold_w = editor_widget_get_fold_gutter_width(self);
    if (fold_w > 0 && x >= (gutter_w - fold_w) && x < gutter_w) {
        return;
    }

    /* Minimap Drag Check */
    if (self->minimap_enabled) {
        int width = gtk_widget_get_width(GTK_WIDGET(self));
        double map_w = self->minimap_width;
        if (map_w > width / 2) map_w = width / 2;

        if (x >= width - map_w) {
            /* Check if drag starts on the lens to determine behavior */
            double height = gtk_widget_get_height(GTK_WIDGET(self));
            double map_content_h, map_scroll_y, map_line_h;
            editor_minimap_get_params(self, height, &map_content_h, &map_scroll_y, &map_line_h);

            /* Calculate lens position to see if drag starts on lens */
            size_t vis_start, vis_end;
            editor_widget_get_visible_line_range(self, &vis_start, &vis_end);

            if (vis_end <= vis_start) vis_end = vis_start + 1;
            double lens_y = (double)vis_start * map_line_h - map_scroll_y;
            double lens_h = (double)(vis_end - vis_start) * map_line_h;

            /* Check if y is within lens bounds */
            if (y >= lens_y && y <= (lens_y + lens_h)) {
                /* Drag starts on lens - enable minimap dragging */
                self->minimap_active = TRUE;
                calculate_minimap_drag_ratio(self);
                gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "default");
                /* Store initial scroll position for relative drag */
                if (self->vadjustment) {
                    self->drag_start_scroll = gtk_adjustment_get_value(self->vadjustment);
                } else {
                    self->drag_start_scroll = 0;
                }

                /* Claim sequence */
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
                return;
            } else {
                /* Drag starts in minimap but not on lens - still claim the sequence to prevent text selection */
                /* If we are already active (from click), update state */
                if (self->minimap_active) {
                     gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "default");
                     /* Claim sequence */
                     gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
                     return;
                }

                /* Otherwise just block selection */
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
                return;
            }
        }
    }

    /* If we just did a multi-click selection (double/triple-click), 
       don't process drag_begin as it would interfere with the selection */
    if (self->multi_click_selection) {
        self->is_dragging_selection = TRUE;  /* Treat as selecting within multi-click */
        return;
    }
    
    /* Check if we're starting a drag on a selection (iterate all cursors) */
    size_t off;
    editor_widget_get_offset_at_point(self, x, y, &off);
    
    gboolean in_selection = FALSE;
    EditorCursor *found_cur = NULL;
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        size_t s = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t e = MAX(cur->cursor_offset, cur->selection_anchor);
        if (s != e && off >= s && off < e) {
            in_selection = TRUE;
            found_cur = cur;
            break;
        }
    }
    
    if (in_selection && found_cur) {
        /* Starting drag on existing selection - prepare for DnD */
        self->is_dragging_selection = TRUE;
        self->drag_start_offset = off;

        /* Create ghost layout for the selected text */
        size_t s = MIN(found_cur->cursor_offset, found_cur->selection_anchor);
        size_t e = MAX(found_cur->cursor_offset, found_cur->selection_anchor);
        
        char *text = document_get_text_range(self->doc, s, e - s);
        if (text) {
            if (self->drag_ghost_layout) g_object_unref(self->drag_ghost_layout);
            self->drag_ghost_layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), text);
            pango_layout_set_font_description(self->drag_ghost_layout, self->font_desc);
            g_free(text);
        }
        self->is_dnd_active = FALSE; /* Will be set to true if drag threshold passed */
    } else {
        /* Normal click/drag - selection handled by on_click_pressed and on_drag_update */
        self->is_dragging_selection = FALSE;
        self->is_dnd_active = FALSE;
    }
    self->is_drag_gesture_active = TRUE;
}

static void
on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc) return;
    
    double start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    
    /* Check if drag started in fold gutter - if so, ignore */
    double gutter_w = get_effective_gutter_width(self);
    double fold_w = editor_widget_get_fold_gutter_width(self);
    if (fold_w > 0 && start_x >= (gutter_w - fold_w) && start_x < gutter_w) {
        return;
    }
    
    /* Guard: If drag was not authorized (e.g. minimap click outside lens, or fold click), return */
    if (!self->is_drag_gesture_active && !self->minimap_active) {
        return;
    }
    
    /* Minimap Drag Update */
    if (self->minimap_active) {
        /* Apply Delta using cached ratio */
        double target_scroll = self->drag_start_scroll + offset_y * self->drag_ratio;
        
        /* Clamp */
        double max_scroll = gtk_adjustment_get_upper(self->vadjustment) - gtk_adjustment_get_page_size(self->vadjustment);
        if (max_scroll < 0) max_scroll = 0;
        
        if (target_scroll < 0) target_scroll = 0;
        if (target_scroll > max_scroll) target_scroll = max_scroll;

        gtk_adjustment_set_value(self->vadjustment, target_scroll);
        return;
    }
    
    self->drag_x = start_x + offset_x;
    self->drag_y = start_y + offset_y;

    size_t off;
    editor_widget_get_offset_at_point(self, self->drag_x, self->drag_y, &off);
    
    gboolean is_dnd_mode = (self->is_dragging_selection && !self->multi_click_selection);

    if (is_dnd_mode) {
        /* Dragging selection for DnD - visual feedback handled in snapshot */
        gboolean has_movement = (fabs(offset_x) > 8 || fabs(offset_y) > 8);

        if (has_movement) {
            self->is_dnd_active = TRUE;

            /* Detect copy mode (Ctrl held) */
            GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
            self->drag_copy_mode = (state & GDK_CONTROL_MASK) != 0;

            /* Change cursor to indicate drag operation */
            if (self->drag_copy_mode) {
                gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "copy"); /* Copy cursor */
            } else {
                gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "default"); /* Default arrow for move */
            }

            /* Calculate drop insertion point */
            size_t drop_off;
            editor_widget_get_offset_at_point(self, self->drag_x, self->drag_y, &drop_off);

            EditorCursor *primary = editor_widget_get_primary_cursor(self);
            size_t sel_start = MIN(primary->cursor_offset, primary->selection_anchor);
            size_t sel_end = MAX(primary->cursor_offset, primary->selection_anchor);

            /* Rule: Never show caret inside or overlap selected range */
            if (drop_off >= sel_start && drop_off < sel_end) {
                self->drag_drop_offset = (size_t)-1; /* Suppress caret */
            } else {
                self->drag_drop_offset = drop_off;
            }
        } else {
            /* Not moved enough yet - suppress feedback */
            self->drag_drop_offset = (size_t)-1;
            self->is_dnd_active = FALSE;
            /* Set cursor to indicate potential drag */
            gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "default"); /* Arrow for potential drag */
        }
    } else {
        /* Standard or Multi-Click Selection Extension */
        update_selection_extension(self, off);
        self->is_dnd_active = FALSE;
        self->drag_drop_offset = (size_t)-1;
        /* During selection extension, show grabbing cursor */
        gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "text"); /* Back to text cursor for selection */
    }
    
    /* Unified Autoscroll Logic */
    gboolean allow_autoscroll = TRUE;
    if (is_dnd_mode && !self->is_dnd_active) allow_autoscroll = FALSE;
    
    if (allow_autoscroll) {
        int widget_height = gtk_widget_get_height(GTK_WIDGET(self));
        int edge_zone = 25; /* Pixels from edge to trigger scroll (reduced from 100) */
        
        if (self->drag_y < edge_zone) {
            /* Top edge */
            double proximity = (edge_zone - self->drag_y) / (double)edge_zone;
            double speed_factor = 0.001 + proximity * proximity * 0.25; 
            double speed = speed_factor * self->line_height;
            start_autoscroll(self, -1, speed);
        } else if (self->drag_y > widget_height - edge_zone) {
            /* Bottom edge */
            double distance_from_edge = widget_height - self->drag_y;
            double proximity = (edge_zone - distance_from_edge) / (double)edge_zone;
            double speed_factor = 0.001 + proximity * proximity * 0.25;
            double speed = speed_factor * self->line_height;
            start_autoscroll(self, 1, speed);
        } else {
            stop_autoscroll(self);
        }
    } else {
        stop_autoscroll(self);
    }
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
editor_widget_drag_drop_finish(EditorWidget *self, size_t drop_off)
{
    if (drop_off == (size_t)-1 || !self->doc) return;

    EditorCursor *primary = editor_widget_get_primary_cursor(self);
    size_t sel_start = MIN(primary->cursor_offset, primary->selection_anchor);
    size_t sel_end = MAX(primary->cursor_offset, primary->selection_anchor);
    size_t sel_len = sel_end - sel_start;
    
    char *text = document_get_text_range(self->doc, sel_start, sel_len);
    if (!text) return;
    
    document_begin_undo_group(self->doc);
    
    /* If moving, delete original first */
    /* Be careful if drop_off is after delete point, it shifts */
    if (!self->drag_copy_mode) {
        document_delete(self->doc, sel_start, sel_len);
        if (drop_off > sel_end) {
            drop_off -= sel_len;
        }
    }
    
    document_insert(self->doc, drop_off, text, sel_len);
    
    /* Select dropped text */
    primary = editor_widget_get_primary_cursor(self);
    primary->selection_anchor = drop_off;
    primary->cursor_offset = drop_off + sel_len;
    self->alt_word_mode = FALSE;
    
    document_end_undo_group(self->doc);
    
    g_free(text);
    
    /* Force update */
    editor_widget_update_adjustments(self, -1, -1);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
on_drag_end(GtkGestureDrag *gesture G_GNUC_UNUSED, double offset_x G_GNUC_UNUSED, double offset_y G_GNUC_UNUSED, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    
    /* Close typing group on mouse interaction */
    editor_widget_finish_typing_undo_group(self);
    
    if (self->minimap_active) {
        self->minimap_active = FALSE;
        /* Reset cursor after minimap drag ends */
        gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "text");
        return;
    }
    
    if (!self->doc) return;
    
    /* Always stop autoscroll when drag ends */
    stop_autoscroll(self);
    
    /* If this was a multi-click selection (double/triple-click), 
       just clear the flag and preserve the selection */
    if (self->multi_click_selection) {
        self->multi_click_selection = FALSE;
        self->is_dragging_selection = FALSE;
        return;
    }
    
    if (self->is_dragging_selection) {
        self->is_dragging_selection = FALSE;
        /* Finalize drag drop? */
        if (self->is_dnd_active && self->drag_drop_offset != (size_t)-1) {
            /* Perform move/copy */
            editor_widget_drag_drop_finish(self, self->drag_drop_offset);
        } else if (!self->is_dnd_active) {
            /* Clicked inside selection without dragging -> Clear selection */
            EditorCursor *primary = editor_widget_get_primary_cursor(self);
            primary->cursor_offset = self->drag_start_offset;
            primary->selection_anchor = self->drag_start_offset;
            self->alt_word_mode = FALSE;
        }
        self->is_dnd_active = FALSE;
        if (self->drag_ghost_layout) {
            g_object_unref(self->drag_ghost_layout);
            self->drag_ghost_layout = NULL;
        }
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
    
    stop_autoscroll(self);
    editor_widget_update_adjustments(self, -1, -1);
    self->is_drag_gesture_active = FALSE;

    /* Reset cursor to default text cursor after drag ends */
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "text");
}

/* Keyboard Input */

void
editor_widget_update_im_cursor_location(EditorWidget *self)
{
    if (!self->im_context || !self->doc) return;
    
    g_signal_emit_by_name(self, "caret-moved");

    /* Get Primary Cursor */
    EditorCursor *primary = editor_widget_get_primary_cursor(self);
    if (!primary) return;
    
    /* SYNC CACHE from Primary Cursor */
    self->cursor_offset = primary->cursor_offset;
    self->selection_anchor = primary->selection_anchor;
    self->target_x = primary->target_x;

    size_t cursor_line = document_get_line_of_offset(self->doc, primary->cursor_offset);
    size_t line_start = document_get_offset_of_line(self->doc, cursor_line);
    size_t char_idx = primary->cursor_offset - line_start;

    char *text; size_t len;
    PangoLayout *layout = create_pango_layout_for_line(self, cursor_line, &text, &len);
    if (!layout) return;

    PangoRectangle strong_pos;
    /* CLAMP index to actual layout length (Pango may have truncated invalid UTF-8) */
    size_t effective_len = strlen(pango_layout_get_text(layout));
    size_t safe_idx = MIN(char_idx, effective_len);
    
    pango_layout_get_cursor_pos(layout, (int)safe_idx, &strong_pos, NULL);

    double cursor_x = pango_units_to_double(strong_pos.x);
    double cursor_y = pango_units_to_double(strong_pos.y);
    double cursor_h = pango_units_to_double(strong_pos.height);
    if (cursor_h < 1) cursor_h = self->line_height;

    g_object_unref(layout); g_free(text);

    double scroll_y = gtk_adjustment_get_value(self->vadjustment);
    double scroll_x = gtk_adjustment_get_value(self->hadjustment);

    double gutter_w = get_effective_gutter_width(self);
    double text_start_x = gutter_w + self->padding_left;

    GdkRectangle rect;
    rect.x = (int)(text_start_x + cursor_x - scroll_x);
    
    /* BOUNDS CHECK: line_y_offsets may not cover all lines for huge files */
    double line_y = 0;
    if (self->line_y_offsets && cursor_line < self->line_y_offsets->len) {
        line_y = self->line_y_offsets->data[cursor_line];
    } else {
        /* Fallback: estimate y position using line height */
        line_y = cursor_line * self->line_height;
    }
    rect.y = (int)(self->padding_top + line_y + cursor_y - scroll_y);
    rect.width = 2; // Cursor width
    rect.height = (int)cursor_h;

    gtk_im_context_set_cursor_location(self->im_context, &rect);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller,
               guint                  keyval,
               guint                  keycode G_GNUC_UNUSED,
               GdkModifierType        state,
               gpointer               user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc) return FALSE;
    
    /* Block all editing if undo/redo is in progress */
    if (self->undo_redo_task) {
        /* Allow Ctrl+Z and Ctrl+Y to be handled (they check the task themselves) */
        if ((keyval == GDK_KEY_z || keyval == GDK_KEY_y) && (state & GDK_CONTROL_MASK)) {
            /* Let it through */
        } else {
            /* Block all other keys */
            return TRUE;
        }
    }
    
    if (gtk_im_context_filter_keypress(self->im_context, gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller))))
        return TRUE;

    /* If key was not handled by IM (e.g. navigation, shortcuts), close any open typing undo group */
    editor_widget_finish_typing_undo_group(self);

    if (keyval == GDK_KEY_Insert) {
        editor_widget_set_insert_mode(self, !self->insert_mode);
        return TRUE;
    }

    gboolean handled = TRUE;

    if (self->is_dragging_selection && (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R)) {
        self->drag_copy_mode = TRUE;
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return TRUE;
    }
    
    switch (keyval) {
        case GDK_KEY_Escape:
            /* standard Reset: remove all extra cursors */
            if (self->cursors->len > 1) {
                g_array_set_size(self->cursors, 1);
                gtk_widget_queue_draw(GTK_WIDGET(self));
            }
            break;
        case GDK_KEY_Up:
            if (state & GDK_ALT_MASK) {
                if (state & GDK_SHIFT_MASK) {
                    editor_widget_add_cursor_vertically(self, -1);
                } else {
                    editor_widget_move_lines_vertically(self, -1);
                }
            } else {
                self->alt_word_mode = FALSE;
                move_cursor(self, -1);
                if (!(state & GDK_SHIFT_MASK)) {
                    for (guint c = 0; c < self->cursors->len; c++) {
                        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                        cur->selection_anchor = cur->cursor_offset;
                    }
                }
            }
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Down:
            if (state & GDK_ALT_MASK) {
                if (state & GDK_SHIFT_MASK) {
                    editor_widget_add_cursor_vertically(self, 1);
                } else {
                    editor_widget_move_lines_vertically(self, 1);
                }
            } else {
                self->alt_word_mode = FALSE;
                move_cursor(self, 1);
                if (!(state & GDK_SHIFT_MASK)) {
                     for (guint c = 0; c < self->cursors->len; c++) {
                         EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                         cur->selection_anchor = cur->cursor_offset;
                     }
                }
            }
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Left:
            if (state & GDK_ALT_MASK) {
                editor_widget_move_selection_horizontally(self, -1);
            } else {
                 self->alt_word_mode = FALSE;
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     if (state & GDK_CONTROL_MASK) {
                         cur->cursor_offset = word_start_or_prev_end_left(self, cur->cursor_offset);
                     } else {
                         cur->cursor_offset = utf8_prev_grapheme(self, cur->cursor_offset);
                     }
                     if (!(state & GDK_SHIFT_MASK)) cur->selection_anchor = cur->cursor_offset;
                 }
                 if (state & GDK_SHIFT_MASK) self->alt_word_mode = FALSE;
                 update_target_x(self);
            }
            editor_widget_update_im_cursor_location(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Right:
            if (state & GDK_ALT_MASK) {
                editor_widget_move_selection_horizontally(self, 1);
            } else {
                 self->alt_word_mode = FALSE;
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     if (state & GDK_CONTROL_MASK) {
                         cur->cursor_offset = word_end_next(self, cur->cursor_offset);
                     } else {
                         cur->cursor_offset = utf8_next_grapheme(self, cur->cursor_offset);
                     }
                     if (!(state & GDK_SHIFT_MASK)) cur->selection_anchor = cur->cursor_offset;
                 }
                 if (state & GDK_SHIFT_MASK) self->alt_word_mode = FALSE;
                 update_target_x(self);
            }
            editor_widget_update_im_cursor_location(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Home:
        {
            for (guint c = 0; c < self->cursors->len; c++) {
                 EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                 if (state & GDK_CONTROL_MASK) {
                     cur->cursor_offset = 0;
                 } else {
                     size_t line = document_get_line_of_offset(self->doc, cur->cursor_offset);
                     cur->cursor_offset = document_get_offset_of_line(self->doc, line);
                 }
                 if (!(state & GDK_SHIFT_MASK)) cur->selection_anchor = cur->cursor_offset;
            }
            update_target_x(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_End:
        {
            for (guint c = 0; c < self->cursors->len; c++) {
                 EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                 if (state & GDK_CONTROL_MASK) {
                     cur->cursor_offset = document_get_length(self->doc);
                 } else {
                     size_t line = document_get_line_of_offset(self->doc, cur->cursor_offset);
                     size_t len;
                     char *t = document_get_line(self->doc, line, &len);
                     g_free(t);
                     size_t start = document_get_offset_of_line(self->doc, line);
                     size_t real_len = len;
                     if (len > 0) {
                          char *last = document_get_text_range(self->doc, start + len - 1, 1);
                          if (last && last[0] == '\n') real_len--;
                          g_free(last);
                     }
                     cur->cursor_offset = start + real_len;
                 }
                 if (!(state & GDK_SHIFT_MASK)) cur->selection_anchor = cur->cursor_offset;
            }
            
            update_target_x(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_Page_Up:
        {
             double page_px = (self->vadjustment) ? gtk_adjustment_get_page_size(self->vadjustment) : 200;
             if (self->vadjustment) {
                 double current = gtk_adjustment_get_value(self->vadjustment);
                 gtk_adjustment_set_value(self->vadjustment, current - page_px);
             }
             int page_lines = (int)(page_px / self->line_height);
             if (page_lines < 1) page_lines = 1;
             move_cursor(self, -page_lines);
             
             if (!(state & GDK_SHIFT_MASK)) {
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     cur->selection_anchor = cur->cursor_offset;
                 }
             }
             scroll_to_cursor(self);
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;
        }
        case GDK_KEY_Page_Down:
        {
             double page_px = (self->vadjustment) ? gtk_adjustment_get_page_size(self->vadjustment) : 200;
             if (self->vadjustment) {
                 double current = gtk_adjustment_get_value(self->vadjustment);
                 gtk_adjustment_set_value(self->vadjustment, current + page_px);
             }
             int page_lines = (int)(page_px / self->line_height);
             if (page_lines < 1) page_lines = 1;
             move_cursor(self, page_lines);
             
             if (!(state & GDK_SHIFT_MASK)) {
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     cur->selection_anchor = cur->cursor_offset;
                 }
             }
             scroll_to_cursor(self);
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;   
        }
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
             /* Enter key */
             document_begin_undo_group(self->doc);
             /* Sort DESC is default for compare_cursors */
             g_array_sort(self->cursors, compare_cursors_desc);
             for (guint c = 0; c < self->cursors->len; c++) {
                 EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                 size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
                 size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
                 
                 /* Auto-indentation logic (per cursor) */
                 char *indent = NULL;
                 if (self->auto_indent) {
                     size_t line = document_get_line_of_offset(self->doc, start);
                     size_t line_len;
                     char *text = document_get_line(self->doc, line, &line_len);
                     if (text) {
                         size_t i = 0;
                         while (i < line_len && (text[i] == ' ' || text[i] == '\t')) i++;
                         if (i > 0) indent = g_strndup(text, i);
                         g_free(text);
                     }
                 }

                 if (start != end) {
                     document_delete(self->doc, start, end-start);
                     cur->cursor_offset = start;
                 }
                 document_insert(self->doc, cur->cursor_offset, "\n", 1);
                 cur->cursor_offset++;
                 
                 if (indent) {
                     size_t len = strlen(indent);
                     document_insert(self->doc, cur->cursor_offset, indent, len);
                     cur->cursor_offset += len;
                     g_free(indent);
                 }
                 
                 cur->selection_anchor = cur->cursor_offset;
             }
             document_end_undo_group(self->doc);
             editor_widget_reset_cursor_blink(self);
             editor_widget_update_adjustments(self, -1, -1);
             scroll_to_cursor(self);
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;
        case GDK_KEY_Tab:
        case GDK_KEY_ISO_Left_Tab:
        {
            gboolean shift = (state & GDK_SHIFT_MASK) || (keyval == GDK_KEY_ISO_Left_Tab);
            
            if (shift) {
                editor_widget_unindent_selection(self);
            } else {
                gboolean any_selection = FALSE;
                for (guint c = 0; c < self->cursors->len; c++) {
                    EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                    if (cur->cursor_offset != cur->selection_anchor) {
                        any_selection = TRUE;
                        break;
                    }
                }
                
                if (any_selection) {
                    editor_widget_indent_selection(self);
                } else {
                    document_begin_undo_group(self->doc);
                    g_array_sort(self->cursors, compare_cursors_desc);
                    
                    char *text_to_insert;
                    char *spaces = NULL;
                    size_t insert_len;
                    
                    if (self->indent_style == 0) {
                        spaces = g_malloc(self->indent_width + 1);
                        memset(spaces, ' ', self->indent_width);
                        spaces[self->indent_width] = '\0';
                        text_to_insert = spaces;
                        insert_len = self->indent_width;
                    } else {
                        text_to_insert = "\t";
                        insert_len = 1;
                    }
                    
                    for (guint c = 0; c < self->cursors->len; c++) {
                        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                        document_insert(self->doc, cur->cursor_offset, text_to_insert, insert_len);
                        cur->cursor_offset += insert_len;
                        cur->selection_anchor = cur->cursor_offset;
                    }
                    
                    if (spaces) g_free(spaces);
                    document_end_undo_group(self->doc);
                }
            }
            
            editor_widget_reset_cursor_blink(self);
            editor_widget_update_adjustments(self, -1, -1);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_BackSpace:
             if (state & GDK_CONTROL_MASK) {
                 /* Ctrl+Backspace: Delete Word */
                 document_begin_undo_group(self->doc);
                 g_array_sort(self->cursors, compare_cursors_desc);
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
                     size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
                     if (start != end) {
                         document_delete(self->doc, start, end-start);
                         cur->cursor_offset = start; cur->selection_anchor = start;
                     } else {
                         size_t prev = word_prev(self, cur->cursor_offset);
                         if (prev < cur->cursor_offset) {
                             document_delete(self->doc, prev, cur->cursor_offset - prev);
                             cur->cursor_offset = prev; cur->selection_anchor = prev;
                         }
                     }
                 }
                 document_end_undo_group(self->doc);
                 editor_widget_update_adjustments(self, -1, -1);
                 scroll_to_cursor(self);
             } else {
                 editor_widget_backspace(self);
             }
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;
        case GDK_KEY_Delete:
             if (state & GDK_CONTROL_MASK) {
                 /* Ctrl+Delete: Delete Word Next */
                 document_begin_undo_group(self->doc);
                 g_array_sort(self->cursors, compare_cursors_desc);
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
                     size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
                     if (start != end) {
                         document_delete(self->doc, start, end-start);
                         cur->cursor_offset = start; cur->selection_anchor = start;
                     } else {
                         size_t next = word_next(self, cur->cursor_offset);
                         if (next > cur->cursor_offset) {
                             document_delete(self->doc, cur->cursor_offset, next - cur->cursor_offset);
                         }
                     }
                 }
                 document_end_undo_group(self->doc);
                 editor_widget_update_adjustments(self, -1, -1);
                 scroll_to_cursor(self);
             } else {
                 editor_widget_delete(self);
             }
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;
        case GDK_KEY_z:
            if (state & GDK_CONTROL_MASK) {
                 editor_widget_undo(self);
            }
            break;
        case GDK_KEY_y:
            if (state & GDK_CONTROL_MASK) {
                 editor_widget_redo(self);
            }
            break;
        case GDK_KEY_c:
            if (state & GDK_CONTROL_MASK) {
                editor_widget_copy(self);
            } else {
                handled = FALSE;
            }
            break;
        case GDK_KEY_x:
            if (state & GDK_CONTROL_MASK) {
                editor_widget_cut(self);
            } else {
                handled = FALSE;
            }
            break;
        case GDK_KEY_v:
            if (state & GDK_CONTROL_MASK) {
                editor_widget_paste(self);
            } else {
                handled = FALSE;
            }
            break;
        case GDK_KEY_a:
             if (state & GDK_CONTROL_MASK) {
                 editor_widget_select_all(self);
             } else {
                handled = FALSE;
            }
            break;
        default:
            handled = FALSE;
            break;
    }
    
    if (handled) {
        size_t line, col;
        editor_widget_get_cursor_position(self, &line, &col);
        g_signal_emit_by_name(self, "cursor-moved", (guint)line, (guint)col);
    }
    
    return handled;
}

static gboolean
on_key_released(GtkEventControllerKey *controller G_GNUC_UNUSED,
                guint                  keyval,
                guint                  keycode G_GNUC_UNUSED,
                GdkModifierType        state G_GNUC_UNUSED,
                gpointer               user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    
    if (self->is_dragging_selection && (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R)) {
        self->drag_copy_mode = FALSE;
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return TRUE;
    }
    
    return FALSE;
}

static void
on_focus_enter (GtkEventControllerFocus *controller G_GNUC_UNUSED,
                gpointer                 user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    gtk_im_context_focus_in(self->im_context);
}

static void
on_focus_leave (GtkEventControllerFocus *controller G_GNUC_UNUSED,
                gpointer                 user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    editor_widget_finish_typing_undo_group(self);
    gtk_im_context_focus_out(self->im_context);
}

static void
on_im_commit(GtkIMContext *context G_GNUC_UNUSED, const char *str, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc || !self->cursors) return;
    
    size_t len = strlen(str);
    if (len == 0) return;

    /* Word-Based Undo Grouping Logic */
    gboolean is_separator = FALSE;
    /* Check if the inserted text starts with a separator (space, punct) */
    if (len > 0) {
        /* Simple heuristic: if first char is not alnum, treat as separator */
        /* Exception: underscore might be part of word? For now, standard alnum check. */
        /* UTF-8 aware check would be better, but basic ASCII check is a start. */
        /* Use glib unicode functions */
        gunichar c = g_utf8_get_char(str);
        if (!g_unichar_isalnum(c) && c != '_') {
            is_separator = TRUE;
        }
    }

    /* Logic:
       1. If !active, start group.
       2. If active:
          - If new is Word AND last was Separator -> Close group, Start new.
          - Else (Word->Word, Sep->Sep, Word->Sep): Continue group.
       3. Update last_char status.
    */
    
    if (!self->typing_undo_group_active) {
        document_begin_undo_group(self->doc);
        self->typing_undo_group_active = TRUE;
    } else {
        if (!is_separator && self->last_char_was_separator) {
            /* Switching from Separator to Word -> New Undo Step */
            document_end_undo_group(self->doc);
            document_begin_undo_group(self->doc);
        }
        /* Else continue current group (Word->Word, Sep->Sep, Word->Sep including trailing spaces) */
    }
    
    self->last_char_was_separator = is_separator;

    /* Basic multi-cursor insert logic for IM commit */
    /* Often IM input implies single cursor, but if we have multiple, we insert at all */
    g_array_sort(self->cursors, compare_cursors_desc);
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        long delta = 0;
        
        if (start != end) {
            document_delete(self->doc, start, end - start);
            cur->cursor_offset = start;
            delta -= (long)(end - start);
        } else if (!self->insert_mode && len > 0) {
            /* Overwrite Mode: Delete next character if not EOL/Newline */
            /* We only overwrite if we are inserting something that isn't a newline itself 
               (though typically IM commit is just text). 
               We simply delete the char at cursor to "replace" it. */
            
            size_t next_off = utf8_next_grapheme(self, cur->cursor_offset);
            if (next_off > cur->cursor_offset) {
                char *existing = document_get_text_range(self->doc, cur->cursor_offset, next_off - cur->cursor_offset);
                gboolean safe_overwrite = TRUE;
                if (existing) {
                    if (existing[0] == '\n' || existing[0] == '\r') safe_overwrite = FALSE;
                    g_free(existing);
                }
                
                if (safe_overwrite) {
                    document_delete(self->doc, cur->cursor_offset, next_off - cur->cursor_offset);
                    /* Cursor stays at start, ready for insert */
                    delta -= (long)(next_off - cur->cursor_offset);
                }
            }
        }
        
        document_insert(self->doc, cur->cursor_offset, str, len);
        cur->cursor_offset += len;
        cur->selection_anchor = cur->cursor_offset;
        delta += (long)len;
        
        if (delta != 0) {
            for (guint j = 0; j < c; j++) {
                EditorCursor *prev = &g_array_index(self->cursors, EditorCursor, j);
                if ((long)prev->cursor_offset + delta < 0) prev->cursor_offset = 0;
                else prev->cursor_offset += delta;
                
                if ((long)prev->selection_anchor + delta < 0) prev->selection_anchor = 0;
                else prev->selection_anchor += delta;
            }
        }
    }
    /* Do NOT allow end undo group here, we keep it open for typing stream */
    // document_end_undo_group(self->doc); 
    
    editor_widget_update_im_cursor_location(self);
    editor_widget_reset_cursor_blink(self);
    editor_widget_update_adjustments(self, -1, -1);
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    
    size_t line, col;
    editor_widget_get_cursor_position(self, &line, &col);
    g_signal_emit_by_name(self, "cursor-moved", (guint)line, (guint)col);
}
