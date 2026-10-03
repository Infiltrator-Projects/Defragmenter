// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_media/test_media_gui.c"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static GtkWidget *find_named(GtkWidget *widget, const char *name)
{
    if (strcmp(gtk_widget_get_name(widget), name) == 0) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *found = NULL;
    for (GList *item = children; item != NULL && found == NULL; item = item->next)
        found = find_named(GTK_WIDGET(item->data), name);
    g_list_free(children);
    return found;
}

static void expand_details(GtkWidget *widget)
{
    if (GTK_IS_EXPANDER(widget))
        gtk_expander_set_expanded(GTK_EXPANDER(widget), TRUE);
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *item = children; item != NULL; item = item->next)
        expand_details(GTK_WIDGET(item->data));
    g_list_free(children);
}

static gboolean layout_probe(gpointer user_data)
{
    unsigned int *phase = user_data;
    GList *windows = gtk_window_list_toplevels();
    GtkWidget *window = NULL;
    for (GList *item = windows; item != NULL; item = item->next)
        if (find_named(GTK_WIDGET(item->data), "ldtm-page-scroll") != NULL)
            window = item->data;
    g_list_free(windows);
    CHECK(window != NULL);
    GtkWidget *page = find_named(window, "ldtm-page-scroll");
    CHECK(GTK_IS_SCROLLED_WINDOW(page));
    CHECK(find_named(window, "ldtm-view-log") != NULL);
    if (*phase == 0U) {
        expand_details(window);
        gtk_window_resize(GTK_WINDOW(window), 900, 680);
        ++*phase;
        return G_SOURCE_CONTINUE;
    }
    gint width = 0, height = 0;
    gtk_window_get_size(GTK_WINDOW(window), &width, &height);
    CHECK(width <= 1024 && height <= 768);
    GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(page));
    CHECK(gtk_adjustment_get_upper(vertical) > gtk_adjustment_get_page_size(vertical));
    gtk_adjustment_set_value(vertical, gtk_adjustment_get_upper(vertical));
    const char *screenshot = g_getenv("LDTM_GUI_TEST_SCREENSHOT");
    if (screenshot != NULL) {
        GdkWindow *surface = gtk_widget_get_window(window);
        GdkPixbuf *pixels = gdk_pixbuf_get_from_window(surface, 0, 0,
            gdk_window_get_width(surface), gdk_window_get_height(surface));
        CHECK(pixels != NULL);
        CHECK(gdk_pixbuf_save(pixels, screenshot, "png", NULL, NULL));
        g_object_unref(pixels);
    }
    gtk_widget_destroy(window);
    return G_SOURCE_REMOVE;
}

static gboolean close_log_dialog(gpointer user_data)
{
    unsigned int *count = user_data;
    GList *windows = gtk_window_list_toplevels();
    for (GList *item = windows; item != NULL; item = item->next) {
        if (GTK_IS_DIALOG(item->data)) {
            ++*count;
            gtk_dialog_response(GTK_DIALOG(item->data), GTK_RESPONSE_CLOSE);
        }
    }
    g_list_free(windows);
    return G_SOURCE_REMOVE;
}

static void test_final_worker_output(void)
{
    LdtmApp app = {0};
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(app.window);
    app.device_store = gtk_list_store_new(LDTM_DEVICE_N_COLUMNS,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING);
    app.device_combo = GTK_COMBO_BOX(make_device_combo(&app));
    app.filesystem_store = gtk_list_store_new(LDTM_FS_N_COLUMNS,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    app.build_button = gtk_button_new();
    app.qualify_button = gtk_button_new();
    app.verify_button = gtk_button_new();
    app.progress = gtk_progress_bar_new();
    app.operation_summary = gtk_label_new("");
    app.readiness_summary = gtk_label_new("");
    app.log_expander = gtk_expander_new("Log");
    app.log_buffer = gtk_text_buffer_new(NULL);
    GtkWidget *owned[] = { GTK_WIDGET(app.device_combo), app.build_button,
        app.qualify_button, app.verify_button, app.progress,
        app.operation_summary, app.readiness_summary, app.log_expander };
    for (size_t index = 0U; index < G_N_ELEMENTS(owned); ++index)
        g_object_ref_sink(owned[index]);
    reset_filesystem_rows(&app);

    int descriptors[2];
    CHECK(pipe(descriptors) == 0);
    for (unsigned int index = 0U; index < 80U; ++index) {
        const char line[] = "worker diagnostic line\n";
        CHECK(write(descriptors[1], line, sizeof(line) - 1U) == (ssize_t)(sizeof(line) - 1U));
    }
    const char final[] = "LDTM_STATUS\tzfs\tqualification-failed\tnlevels=2 indblkshift=11\nFINAL DIAGNOSTIC\n";
    CHECK(write(descriptors[1], final, sizeof(final) - 1U) == (ssize_t)(sizeof(final) - 1U));
    CHECK(close(descriptors[1]) == 0);
    app.stdout_channel = g_io_channel_unix_new(descriptors[0]);
    g_io_channel_set_close_on_unref(app.stdout_channel, TRUE);
    CHECK(g_io_channel_set_flags(app.stdout_channel, G_IO_FLAG_NONBLOCK, NULL) == G_IO_STATUS_NORMAL);
    CHECK(channel_watch(app.stdout_channel, G_IO_IN, &app));

    unsigned int dialogs = 0U;
    g_idle_add(close_log_dialog, &dialogs);
    worker_finished(0, 1 << 8, &app);
    CHECK(dialogs == 1U);
    CHECK(gtk_expander_get_expanded(GTK_EXPANDER(app.log_expander)));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(app.log_buffer, &start, &end);
    char *log = gtk_text_buffer_get_text(app.log_buffer, &start, &end, FALSE);
    CHECK(strstr(log, "FINAL DIAGNOSTIC") != NULL);
    CHECK(strstr(log, "exit status 1") != NULL);
    CHECK(strcmp(display_result("qualification-failed"), "Qualification failed") == 0);
    GtkTreeIter iter;
    CHECK(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.filesystem_store), &iter));
    gboolean found = FALSE;
    do {
        char *key = NULL, *result = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app.filesystem_store), &iter,
                           LDTM_FS_COL_KEY, &key, LDTM_FS_COL_RESULT, &result, -1);
        if (strcmp(key, "zfs") == 0) {
            CHECK(strcmp(result, "Qualification failed") == 0);
            found = TRUE;
        }
        g_free(key);
        g_free(result);
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app.filesystem_store), &iter));
    CHECK(found);
    copy_operation_log(&app);
    char *copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    CHECK(g_strcmp0(log, copied) == 0);
    g_free(copied);
    g_free(log);
    gtk_widget_destroy(app.window);
    g_object_unref(app.window);
    for (size_t index = 0U; index < G_N_ELEMENTS(owned); ++index) {
        gtk_widget_destroy(owned[index]);
        g_object_unref(owned[index]);
    }
    g_object_unref(app.device_store);
    g_object_unref(app.filesystem_store);
    g_object_unref(app.log_buffer);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    ldtm_apply_mb_theme();
    test_final_worker_output();
    unsigned int phase = 0U;
    g_timeout_add(100U, layout_probe, &phase);
    CHECK(ldtm_gui_main(argc, argv) == 0);
    CHECK(phase == 1U);
    return 0;
}
