// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_media/test_media_gui.c"
#include <infiltratr/design.h>

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

static void check_font(GtkWidget *widget, const char *family, int minimum_pixels)
{
    PangoFontDescription *requested = NULL;
    CHECK(widget != NULL);
    gtk_style_context_get(gtk_widget_get_style_context(widget), GTK_STATE_FLAG_NORMAL,
                          "font", &requested, NULL);
    CHECK(requested != NULL);
    CHECK(g_strcmp0(pango_font_description_get_family(requested), family) == 0);
    double pixels = (double)pango_font_description_get_size(requested) / PANGO_SCALE;
    if (!pango_font_description_get_size_is_absolute(requested)) {
        double resolution = gdk_screen_get_resolution(gtk_widget_get_screen(widget));
        if (resolution <= 0.0) resolution = 96.0;
        pixels *= resolution / 72.0;
    }
    CHECK(pixels >= (double)minimum_pixels - 0.1);
    if (g_getenv("LDTM_GUI_REQUIRE_MB_FONTS") != NULL) {
        PangoFont *font = pango_context_load_font(gtk_widget_get_pango_context(widget), requested);
        CHECK(font != NULL);
        PangoFontDescription *resolved = pango_font_describe(font);
        CHECK(g_strcmp0(pango_font_description_get_family(resolved), family) == 0);
        CHECK(pango_font_description_get_weight(resolved) == pango_font_description_get_weight(requested));
        pango_font_description_free(resolved);
        g_object_unref(font);
    }
    pango_font_description_free(requested);
}

static void capture_window(GtkWidget *window, const char *environment_key)
{
    const char *screenshot = g_getenv(environment_key);
    if (screenshot == NULL) return;
    GdkWindow *surface = gtk_widget_get_window(window);
    GdkPixbuf *pixels = gdk_pixbuf_get_from_window(surface, 0, 0,
        gdk_window_get_width(surface), gdk_window_get_height(surface));
    CHECK(pixels != NULL);
    CHECK(gdk_pixbuf_save(pixels, screenshot, "png", NULL, NULL));
    g_object_unref(pixels);
}

static void check_operation_colours(void)
{
    const char *classes[] = { "ldtm-operation-cyan", "ldtm-operation-purple", "ldtm-operation-red" };
    const double expected[][3] = { {49.0/255.0, 200.0/255.0, 244.0/255.0},
        {179.0/255.0, 108.0/255.0, 1.0}, {1.0, 107.0/255.0, 107.0/255.0} };
    for (size_t index = 0U; index < G_N_ELEMENTS(classes); ++index) {
        GtkWidget *well = make_icon_well("emblem-ok-symbolic", classes[index], 24);
        g_object_ref_sink(well);
        GList *children = gtk_container_get_children(GTK_CONTAINER(well));
        CHECK(children != NULL);
        GtkWidget *image = GTK_WIDGET(children->data);
        g_list_free(children);
        GdkRGBA colour;
        gtk_style_context_get_color(gtk_widget_get_style_context(image), GTK_STATE_FLAG_NORMAL, &colour);
        CHECK(colour.red >= expected[index][0] - 0.01 && colour.red <= expected[index][0] + 0.01);
        CHECK(colour.green >= expected[index][1] - 0.01 && colour.green <= expected[index][1] + 0.01);
        CHECK(colour.blue >= expected[index][2] - 0.01 && colour.blue <= expected[index][2] + 0.01);
        gtk_widget_destroy(well);
        g_object_unref(well);
    }
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
        const InfiltratrTypography *typography = infiltratr_typography();
        check_font(find_named(window, "ldtm-header-brand-title"), typography->brand_family, 22);
        check_font(find_named(window, "ldtm-hero-title"), typography->brand_family, 30);
        check_font(find_named(window, "ldtm-hero-subtitle"), typography->ui_family, 12);
        check_font(find_named(window, "ldtm-section-title"), typography->ui_family, 16);
        GdkRectangle workarea;
        GdkWindow *surface = gtk_widget_get_window(window);
        GdkMonitor *monitor = gdk_display_get_monitor_at_window(gtk_widget_get_display(window), surface);
        gdk_monitor_get_workarea(monitor, &workarea);
        CHECK(gdk_window_get_width(surface) <= workarea.width);
        CHECK(gdk_window_get_height(surface) <= workarea.height);
        GtkWidget *grid = find_named(window, "ldtm-filesystem-grid");
        CHECK(GTK_IS_FLOW_BOX(grid));
        CHECK(!GTK_IS_SCROLLED_WINDOW(gtk_widget_get_parent(grid)));
        GList *tiles = gtk_container_get_children(GTK_CONTAINER(grid));
        CHECK(g_list_length(tiles) == LDTM_SPEC_COUNT);
        for (GList *item = tiles; item != NULL; item = item->next)
            CHECK(gtk_widget_get_allocated_height(GTK_WIDGET(item->data)) >= 48);
        g_list_free(tiles);
        capture_window(window, "LDTM_GUI_TEST_DESKTOP_SCREENSHOT");
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
    if (*phase == 1U) {
        ++*phase;
        return G_SOURCE_CONTINUE;
    }
    capture_window(window, "LDTM_GUI_TEST_SCREENSHOT");
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
    check_operation_colours();
    test_final_worker_output();
    unsigned int phase = 0U;
    g_timeout_add(100U, layout_probe, &phase);
    CHECK(ldtm_gui_main(argc, argv) == 0);
    CHECK(phase == 2U);
    return 0;
}
