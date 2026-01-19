#include "filter-bar.h"
#include "document.h"

struct _ViteFilterBar {
    GtkBox parent_instance;

    EditorWidget *editor;

    /* UI Elements */
    GtkWidget *filter_entry;
    GtkWidget *results_label;
    GtkWidget *clear_btn;

    GtkWidget *regex_check;
    GtkWidget *case_check;

    /* Filter Result */
    FilterResult *current_filter_result;
    gboolean filter_active;
    
    /* Async Task */
    DocumentFilterTask *current_task;
    guint filter_tick_id;
};

G_DEFINE_TYPE(ViteFilterBar, vite_filter_bar, GTK_TYPE_BOX)

static void vite_filter_bar_dispose(GObject *object) {
    ViteFilterBar *self = VITE_FILTER_BAR(object);
    
    if (self->current_filter_result) {
        filter_result_free(self->current_filter_result);
        self->current_filter_result = NULL;
    }
    
    if (self->filter_tick_id > 0) {
        g_source_remove(self->filter_tick_id);
        self->filter_tick_id = 0;
    }
    
    if (self->current_task) {
        document_filter_async_cancel(self->current_task);
        self->current_task = NULL;
    }

    G_OBJECT_CLASS(vite_filter_bar_parent_class)->dispose(object);
}

static void update_results_label(ViteFilterBar *self) {
    if (!self) return;
    
    if (!self->current_filter_result || !self->filter_active) {
        if (self->current_task) {
            gtk_label_set_text(GTK_LABEL(self->results_label), "Filtering...");
            gtk_widget_set_visible(self->results_label, TRUE);
        } else {
            gtk_widget_set_visible(self->results_label, FALSE);
        }
        return;
    }

    size_t count = self->current_filter_result->count;

    if (count == 0) {
        gtk_label_set_text(GTK_LABEL(self->results_label), "No matches");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "%zu matching lines", count);
        gtk_label_set_text(GTK_LABEL(self->results_label), buf);
    }
    gtk_widget_set_visible(self->results_label, TRUE);
}

static gboolean filter_async_step(gpointer user_data) {
    ViteFilterBar *self = VITE_FILTER_BAR(user_data);
    
    if (!self->current_task) {
        self->filter_tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    /* Run for up to 2ms */
    gboolean finished = document_filter_async_step(self->current_task, 2000);
    
    if (finished) {
        /* Get results */
        FilterResult *res = document_filter_async_finish(self->current_task);
        self->current_task = NULL;
        self->filter_tick_id = 0;
        
        self->current_filter_result = res;
        self->filter_active = TRUE;
        
        /* Update Editor */
        const char *pattern = gtk_editable_get_text(GTK_EDITABLE(self->filter_entry));
        gboolean regex = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->regex_check));
        gboolean case_sensitive = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->case_check));
        
        if (res->count > 0) {
            editor_widget_set_filtered_lines(self->editor, 
                                           res->lines, 
                                           res->count,
                                           pattern, regex, case_sensitive);
        } else {
            editor_widget_set_filtered_lines(self->editor, NULL, 0, NULL, FALSE, FALSE);
        }
        
        update_results_label(self);
        return G_SOURCE_REMOVE;
    }
    
    return G_SOURCE_CONTINUE;
}

static gboolean apply_filter(ViteFilterBar *self) {
    /* Cancel any running task */
    if (self->filter_tick_id > 0) {
        g_source_remove(self->filter_tick_id);
        self->filter_tick_id = 0;
    }
    
    if (self->current_task) {
        document_filter_async_cancel(self->current_task);
        self->current_task = NULL;
    }

    /* Free previous filter result */
    if (self->current_filter_result) {
        filter_result_free(self->current_filter_result);
        self->current_filter_result = NULL;
    }

    Document *doc = editor_widget_get_document(self->editor);
    if (!doc) {
        self->filter_active = FALSE;
        editor_widget_set_filtered_lines(self->editor, NULL, 0, NULL, FALSE, FALSE);
        update_results_label(self);
        return G_SOURCE_REMOVE;
    }

    const char *pattern = gtk_editable_get_text(GTK_EDITABLE(self->filter_entry));
    if (!pattern || !*pattern) {
        /* Empty pattern means show all lines */
        self->filter_active = FALSE;
        editor_widget_set_filtered_lines(self->editor, NULL, 0, NULL, FALSE, FALSE);
        gtk_widget_set_visible(self->results_label, FALSE);
        return G_SOURCE_REMOVE;
    }

    gboolean regex = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->regex_check));
    gboolean case_sensitive = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->case_check));

    /* Start Async Task */
    self->current_task = document_filter_async_start(doc, pattern, regex, case_sensitive);
    if (self->current_task) {
        self->filter_tick_id = g_idle_add(filter_async_step, self);
        update_results_label(self); /* Shows "Filtering..." */
    }

    return G_SOURCE_REMOVE;
}

static void on_filter_changed(GtkWidget *widget, gpointer user_data) {
    ViteFilterBar *self = VITE_FILTER_BAR(user_data);
    apply_filter(self);
}

static void on_clear_clicked(GtkButton *btn, gpointer user_data) {
    ViteFilterBar *self = VITE_FILTER_BAR(user_data);
    vite_filter_bar_clear_filter(self);
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    ViteFilterBar *self = VITE_FILTER_BAR(user_data);
    if (keyval == GDK_KEY_Escape) {
        vite_filter_bar_close(self);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        apply_filter(self);
        return TRUE;
    }
    return FALSE;
}

static void vite_filter_bar_class_init(ViteFilterBarClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = vite_filter_bar_dispose;

    gtk_widget_class_set_css_name(widget_class, "filterbar");
}

static void vite_filter_bar_init(ViteFilterBar *self) {
    /* Initialize fields */
    self->current_filter_result = NULL;
    self->current_task = NULL;
    self->filter_tick_id = 0;
    self->filter_active = FALSE;
}

GtkWidget *vite_filter_bar_new(EditorWidget *editor) {
    ViteFilterBar *self = g_object_new(VITE_TYPE_FILTER_BAR, NULL);
    self->editor = editor;

    gtk_widget_add_css_class(GTK_WIDGET(self), "filter-bar");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 0);

    /* Main Layout */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(self), row);

    /* Filter Entry */
    self->filter_entry = gtk_entry_new();
    gtk_widget_set_hexpand(self->filter_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->filter_entry), "Filter lines (Ctrl+Alt+F)");
    g_signal_connect(self->filter_entry, "changed", G_CALLBACK(on_filter_changed), self);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(self->filter_entry, key_ctrl);
    gtk_box_append(GTK_BOX(row), self->filter_entry);

    /* Results Label */
    self->results_label = gtk_label_new("");
    gtk_widget_add_css_class(self->results_label, "dim-label");
    gtk_widget_add_css_class(self->results_label, "caption");
    gtk_box_append(GTK_BOX(row), self->results_label);

    /* Options Menu */
    GtkWidget *options_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(options_btn), "system-run-symbolic");
    gtk_widget_add_css_class(options_btn, "flat");

    GtkWidget *popover = gtk_popover_new();
    GtkWidget *pop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(pop_box, 12);
    gtk_widget_set_margin_bottom(pop_box, 12);
    gtk_widget_set_margin_start(pop_box, 12);
    gtk_widget_set_margin_end(pop_box, 12);

    self->regex_check = gtk_check_button_new_with_label("Regular Expressions");
    g_signal_connect(self->regex_check, "toggled", G_CALLBACK(on_filter_changed), self);
    gtk_box_append(GTK_BOX(pop_box), self->regex_check);

    self->case_check = gtk_check_button_new_with_label("Case Sensitive");
    g_signal_connect(self->case_check, "toggled", G_CALLBACK(on_filter_changed), self);
    gtk_box_append(GTK_BOX(pop_box), self->case_check);

    gtk_popover_set_child(GTK_POPOVER(popover), pop_box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(options_btn), popover);
    gtk_box_append(GTK_BOX(row), options_btn);

    /* Clear Button */
    self->clear_btn = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_add_css_class(self->clear_btn, "flat");
    gtk_widget_set_tooltip_text(self->clear_btn, "Clear Filter");
    g_signal_connect(self->clear_btn, "clicked", G_CALLBACK(on_clear_clicked), self);
    gtk_box_append(GTK_BOX(row), self->clear_btn);

    /* Close Button */
    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close_btn, "flat");
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(vite_filter_bar_close), self);
    gtk_box_append(GTK_BOX(row), close_btn);

    return GTK_WIDGET(self);
}

void vite_filter_bar_show(ViteFilterBar *bar) {
    gtk_widget_set_visible(GTK_WIDGET(bar), TRUE);
    gtk_widget_grab_focus(bar->filter_entry);
}

void vite_filter_bar_close(ViteFilterBar *bar) {
    gtk_widget_set_visible(GTK_WIDGET(bar), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(bar->filter_entry), "");
    
    /* Clear the filter in the editor */
    bar->filter_active = FALSE;
    if (bar->current_filter_result) {
        filter_result_free(bar->current_filter_result);
        bar->current_filter_result = NULL;
    }
    editor_widget_set_filtered_lines(bar->editor, NULL, 0, NULL, FALSE, FALSE);
    
    gtk_widget_grab_focus(GTK_WIDGET(bar->editor));
}

void vite_filter_bar_apply_filter(ViteFilterBar *bar) {
    apply_filter(bar);
}

void vite_filter_bar_clear_filter(ViteFilterBar *bar) {
    gtk_editable_set_text(GTK_EDITABLE(bar->filter_entry), "");
    bar->filter_active = FALSE;
    if (bar->current_task) {
        document_filter_async_cancel(bar->current_task);
        bar->current_task = NULL;
    }
    if (bar->filter_tick_id > 0) {
        g_source_remove(bar->filter_tick_id);
        bar->filter_tick_id = 0;
    }

    if (bar->current_filter_result) {
        filter_result_free(bar->current_filter_result);
        bar->current_filter_result = NULL;
    }
    editor_widget_set_filtered_lines(bar->editor, NULL, 0, NULL, FALSE, FALSE);
    gtk_widget_set_visible(bar->results_label, FALSE);
}

void vite_filter_bar_set_text(ViteFilterBar *bar, const char *text) {
    g_return_if_fail(VITE_IS_FILTER_BAR(bar));
    gtk_editable_set_text(GTK_EDITABLE(bar->filter_entry), text ? text : "");
}