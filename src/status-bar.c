#include "status-bar.h"

struct _ViteStatusBar {
    GtkWidget parent_instance;
    GtkWidget *box;
    
    GtkWidget *file_type_btn;
    GtkWidget *file_type_label;
    
    GtkWidget *tab_width_btn;
    GtkWidget *tab_width_label;
    
    GtkWidget *encoding_btn;
    GtkWidget *encoding_label;
    GSimpleActionGroup *encoding_group;
    
    GtkWidget *line_ending_btn;
    GtkWidget *line_ending_label;
    GSimpleActionGroup *line_ending_group;
    
    GtkWidget *cursor_label;
    GtkWidget *ins_label;
};

G_DEFINE_TYPE(ViteStatusBar, vite_status_bar, GTK_TYPE_WIDGET)

enum {
    FILE_TYPE_CHANGED,
    LINE_ENDING_CHANGED,
    ENCODING_CHANGED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
vite_status_bar_dispose(GObject *object)
{
    ViteStatusBar *self = VITE_STATUS_BAR(object);
    g_clear_pointer(&self->box, gtk_widget_unparent);
    G_OBJECT_CLASS(vite_status_bar_parent_class)->dispose(object);
}

static void
vite_status_bar_class_init(ViteStatusBarClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    
    object_class->dispose = vite_status_bar_dispose;
    
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
    gtk_widget_class_set_css_name(widget_class, "statusbar");
    
    signals[FILE_TYPE_CHANGED] = g_signal_new("file-type-changed",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[LINE_ENDING_CHANGED] = g_signal_new("line-ending-changed",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE, 1, G_TYPE_STRING);
                                 
    signals[ENCODING_CHANGED] = g_signal_new("encoding-changed",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

/* ... existing helper functions ... */

static void
on_line_ending_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteStatusBar *self = VITE_STATUS_BAR(user_data);
    g_simple_action_set_state(action, parameter);
    
    const char *id = g_variant_get_string(parameter, NULL);

    
    vite_status_bar_set_line_ending(self, id);
    g_signal_emit(self, signals[LINE_ENDING_CHANGED], 0, id);
}

static void
create_line_ending_menu(ViteStatusBar *self)
{
    GMenu *menu = g_menu_new();
    
    self->line_ending_group = g_simple_action_group_new();
    GActionEntry entries[] = {
        { "set", NULL, "s", "'lf'", on_line_ending_activated }
    };
    g_action_map_add_action_entries(G_ACTION_MAP(self->line_ending_group), entries, G_N_ELEMENTS(entries), self);
    gtk_widget_insert_action_group(self->line_ending_btn, "le", G_ACTION_GROUP(self->line_ending_group));
    
    g_menu_append(menu, "Unix/Linux (LF)", "le.set::lf");
    g_menu_append(menu, "Windows (CRLF)", "le.set::crlf");
    g_menu_append(menu, "Legacy Mac (CR)", "le.set::cr");
    
    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->line_ending_btn), popover);
    g_object_unref(menu);
}

static void
on_encoding_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteStatusBar *self = VITE_STATUS_BAR(user_data);
    g_simple_action_set_state(action, parameter);
    
    const char *id = g_variant_get_string(parameter, NULL);

    
    vite_status_bar_set_encoding(self, id);
    g_signal_emit(self, signals[ENCODING_CHANGED], 0, id);
}

static void
create_encoding_menu(ViteStatusBar *self)
{
    GMenu *menu = g_menu_new();
    
    self->encoding_group = g_simple_action_group_new();
    GActionEntry entries[] = {
        { "set", NULL, "s", "'utf-8'", on_encoding_activated }
    };
    g_action_map_add_action_entries(G_ACTION_MAP(self->encoding_group), entries, G_N_ELEMENTS(entries), self);
    gtk_widget_insert_action_group(self->encoding_btn, "enc", G_ACTION_GROUP(self->encoding_group));
    
    g_menu_append(menu, "UTF-8", "enc.set::utf-8");
    g_menu_append(menu, "UTF-16 LE", "enc.set::utf-16le");
    g_menu_append(menu, "UTF-16 BE", "enc.set::utf-16be");
    
    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->encoding_btn), popover);
    g_object_unref(menu);
}

static GtkWidget *
create_separator(void)
{
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_top(sep, 4);
    gtk_widget_set_margin_bottom(sep, 4);
    gtk_widget_set_margin_start(sep, 4);
    gtk_widget_set_margin_end(sep, 4);
    return sep;
}

static GtkWidget *
create_menu_button(ViteStatusBar *self, GtkWidget **label_out, const char *tooltip, const char *default_text)
{
    GtkWidget *btn = gtk_menu_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_tooltip_text(btn, tooltip);
    
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), default_text);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(btn), label);
    
    if (label_out) *label_out = label;
    
    return btn;
}

/* ... existing file type stuff ... */

typedef struct {
    const char *name;
    const char *id;
} FileTypeEntry;

static const FileTypeEntry file_types[] = {
    { "Plain Text", NULL },
    { "C", "c" },
    { "C++", "cpp" },
    { "Python", "python" },
    { "Bash", "bash" },
    { "Rust", "rust" },
    { "Header", "h" },
    { NULL, NULL }
};

static void
on_file_type_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ViteStatusBar *self = VITE_STATUS_BAR(user_data);
    const char *id = g_object_get_data(G_OBJECT(row), "lang-id");
    const char *name = g_object_get_data(G_OBJECT(row), "lang-name");
    
    /* Update label */
    vite_status_bar_set_file_type(self, name);
    
    /* Emit signal */
    g_signal_emit(self, signals[FILE_TYPE_CHANGED], 0, id);
    
    /* Close popover */
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(box), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
}

static gboolean
file_type_filter_func(GtkListBoxRow *row, gpointer user_data)
{
    GtkEditable *entry = GTK_EDITABLE(user_data);
    const char *text = gtk_editable_get_text(entry);
    if (!text || !text[0]) return TRUE;
    
    const char *name = g_object_get_data(G_OBJECT(row), "lang-name");
    if (!name) return FALSE;
    
    /* Case insensitive search */
    char *name_lower = g_utf8_strdown(name, -1);
    char *text_lower = g_utf8_strdown(text, -1);
    gboolean match = (strstr(name_lower, text_lower) != NULL);
    g_free(name_lower);
    g_free(text_lower);
    
    return match;
}

static void
on_search_changed(GtkEditable *entry, gpointer user_data)
{
    GtkListBox *listbox = GTK_LIST_BOX(user_data);
    gtk_list_box_invalidate_filter(listbox);
}

static void
create_file_type_menu(ViteStatusBar *self)
{
    GtkWidget *popover = gtk_popover_new();
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->file_type_btn), popover);
    
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    
    /* Search Entry */
    GtkWidget *entry = gtk_search_entry_new();
    gtk_box_append(GTK_BOX(box), entry);
    
    /* List Box in Scrolled Window */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scrolled), 200);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 200);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(box), scrolled);
    
    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(listbox, "boxed-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), listbox);
    
    for (int i = 0; file_types[i].name != NULL; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new(file_types[i].name);
        gtk_widget_set_margin_start(lbl, 12);
        gtk_widget_set_margin_end(lbl, 12);
        gtk_widget_set_margin_top(lbl, 8);
        gtk_widget_set_margin_bottom(lbl, 8);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        
        g_object_set_data(G_OBJECT(row), "lang-name", (gpointer)file_types[i].name);
        g_object_set_data(G_OBJECT(row), "lang-id", (gpointer)file_types[i].id);
        
        gtk_list_box_append(GTK_LIST_BOX(listbox), row);
    }
    
    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_file_type_row_activated), self);
    
    gtk_list_box_set_filter_func(GTK_LIST_BOX(listbox), file_type_filter_func, entry, NULL);
    g_signal_connect(entry, "search-changed", G_CALLBACK(on_search_changed), listbox);
}

static void
vite_status_bar_init(ViteStatusBar *self)
{
    self->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_parent(self->box, GTK_WIDGET(self));
    
    gtk_widget_add_css_class(GTK_WIDGET(self), "status-bar");
    
    /* File Type */
    self->file_type_btn = create_menu_button(self, &self->file_type_label, "File Type", "Plain Text");
    create_file_type_menu(self); /* Setup Popover */
    gtk_box_append(GTK_BOX(self->box), self->file_type_btn);
    gtk_box_append(GTK_BOX(self->box), create_separator());
    
    /* Tab Width */
    self->tab_width_btn = create_menu_button(self, &self->tab_width_label, "Tab Width", "Tab: 4");
    gtk_box_append(GTK_BOX(self->box), self->tab_width_btn);
    gtk_box_append(GTK_BOX(self->box), create_separator());

    /* Encoding */
    self->encoding_btn = create_menu_button(self, &self->encoding_label, "Encoding", "UTF-8");
    create_encoding_menu(self);
    gtk_box_append(GTK_BOX(self->box), self->encoding_btn);
    gtk_box_append(GTK_BOX(self->box), create_separator());

    /* Line Ending */
    /* Line Ending */
    self->line_ending_btn = create_menu_button(self, &self->line_ending_label, "Line Ending", "LF");
    create_line_ending_menu(self);
    gtk_box_append(GTK_BOX(self->box), self->line_ending_btn);
    
    /* Spacer */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(self->box), spacer);
    
    /* Cursor Position */
    self->cursor_label = gtk_label_new("Ln 1, Col 1");
    gtk_widget_set_margin_start(self->cursor_label, 8);
    gtk_widget_set_margin_end(self->cursor_label, 8);
    gtk_box_append(GTK_BOX(self->box), self->cursor_label);
    
    gtk_box_append(GTK_BOX(self->box), create_separator());
    
    /* INS/OVR Button */
    self->ins_label = gtk_label_new("INS");
    /* Make it a button */
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_button_set_child(GTK_BUTTON(btn), self->ins_label);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(btn), "win.toggle-insert-mode");
    
    gtk_widget_set_margin_start(btn, 4);
    gtk_widget_set_margin_end(btn, 4);
    
    gtk_box_append(GTK_BOX(self->box), btn);
}

GtkWidget *
vite_status_bar_new(void)
{
    return g_object_new(VITE_TYPE_STATUS_BAR, NULL);
}

void
vite_status_bar_set_insert_mode(ViteStatusBar *self, gboolean insert)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    gtk_label_set_text(GTK_LABEL(self->ins_label), insert ? "INS" : "OVR");
    /* Optional: Style OVR differently? */
}

void
vite_status_bar_set_cursor_position(ViteStatusBar *self, int line, int col)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    char *text = g_strdup_printf("Ln %d, Col %d", line + 1, col + 1);
    gtk_label_set_text(GTK_LABEL(self->cursor_label), text);
    g_free(text);
}

void
vite_status_bar_set_file_type(ViteStatusBar *self, const char *file_type)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    if (!file_type) file_type = "Plain Text";
    /* Bold if not plain text? svite does this. */
    char *markup = g_strdup_printf("<span font_weight='normal'>%s</span>", file_type);
    gtk_label_set_markup(GTK_LABEL(self->file_type_label), markup);
    g_free(markup);
}


void
vite_status_bar_set_encoding(ViteStatusBar *self, const char *encoding_id)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    if (!encoding_id) encoding_id = "utf-8";
    
    /* Map ID to Display Name */
    const char *display = "UTF-8";
    if (g_strcmp0(encoding_id, "utf-16le") == 0) display = "UTF-16 LE";
    else if (g_strcmp0(encoding_id, "utf-16be") == 0) display = "UTF-16 BE";
    
    char *markup = g_strdup_printf("<span font_weight='normal'>%s</span>", display);
    gtk_label_set_markup(GTK_LABEL(self->encoding_label), markup);
    g_free(markup);
    
    /* Update Action State directly using ID */
    if (self->encoding_group) {
        GAction *act = g_action_map_lookup_action(G_ACTION_MAP(self->encoding_group), "set");
        if (act) {
            g_simple_action_set_state(G_SIMPLE_ACTION(act), g_variant_new_string(encoding_id));
        }
    }
}

void
vite_status_bar_set_line_ending(ViteStatusBar *self, const char *line_ending_id)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    if (!line_ending_id) line_ending_id = "lf";
    
    /* Map ID to Display Name */
    const char *display = "LF";
    if (g_strcmp0(line_ending_id, "crlf") == 0) display = "CRLF";
    else if (g_strcmp0(line_ending_id, "cr") == 0) display = "CR";
    
    char *markup = g_strdup_printf("<span font_weight='normal'>%s</span>", display);
    gtk_label_set_markup(GTK_LABEL(self->line_ending_label), markup);
    g_free(markup);
    
    /* Update Action State directly using ID */
    if (self->line_ending_group) {
        GAction *act = g_action_map_lookup_action(G_ACTION_MAP(self->line_ending_group), "set");
        if (act) {
            g_simple_action_set_state(G_SIMPLE_ACTION(act), g_variant_new_string(line_ending_id));
        }
    }
}

void
vite_status_bar_set_tab_width(ViteStatusBar *self, int width)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    char *markup = g_strdup_printf("<span font_weight='normal'>Tab: %d</span>", width);
    gtk_label_set_markup(GTK_LABEL(self->tab_width_label), markup);
    g_free(markup);
}

