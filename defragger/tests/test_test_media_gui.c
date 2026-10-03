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

static void test_final_worker_output(LdtmApp *app)
{
    int descriptors[2];
    CHECK(pipe(descriptors) == 0);
    for (unsigned int index = 0U; index < 80U; ++index) {
        const char line[] = "worker diagnostic line\n";
        CHECK(write(descriptors[1], line, sizeof(line) - 1U) == (ssize_t)(sizeof(line) - 1U));
    }
    const char final[] = "LDTM_STATUS\tzfs\tqualification-failed\tnlevels=2 indblkshift=11\nFINAL DIAGNOSTIC\n";
    CHECK(write(descriptors[1], final, sizeof(final) - 1U) == (ssize_t)(sizeof(final) - 1U));
    CHECK(close(descriptors[1]) == 0);
    app->stdout_channel = g_io_channel_unix_new(descriptors[0]);
    g_io_channel_set_close_on_unref(app->stdout_channel, TRUE);
    CHECK(g_io_channel_set_flags(app->stdout_channel, G_IO_FLAG_NONBLOCK, NULL) == G_IO_STATUS_NORMAL);
    CHECK(channel_watch(app->stdout_channel, G_IO_IN, app));

    unsigned int dialogs = 0U;
    g_idle_add(close_log_dialog, &dialogs);
    worker_finished(0, 1 << 8, app);
    CHECK(dialogs == 1U);
    CHECK(g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(app->result_stack)), "log") == 0);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(app->log_buffer, &start, &end);
    char *log = gtk_text_buffer_get_text(app->log_buffer, &start, &end, FALSE);
    CHECK(strstr(log, "FINAL DIAGNOSTIC") != NULL);
    CHECK(strstr(log, "exit status 1") != NULL);
    CHECK(strcmp(display_result("qualification-failed"), "Qualification failed") == 0);
    GtkTreeIter iter;
    CHECK(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->filesystem_store), &iter));
    gboolean found = FALSE;
    do {
        char *key = NULL, *result = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->filesystem_store), &iter,
                           LDTM_FS_COL_KEY, &key, LDTM_FS_COL_RESULT, &result, -1);
        if (strcmp(key, "zfs") == 0) {
            CHECK(strcmp(result, "Qualification failed") == 0);
            found = TRUE;
        }
        g_free(key);
        g_free(result);
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->filesystem_store), &iter));
    CHECK(found);
    copy_operation_log(app);
    char *copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    CHECK(g_strcmp0(log, copied) == 0);
    g_free(copied);
    g_free(log);
}

static void check_inside(GtkWidget *window, GtkWidget *widget)
{
    gint x = 0, y = 0;
    CHECK(gtk_widget_translate_coordinates(widget, window, 0, 0, &x, &y));
    CHECK(x >= 0 && y >= 0);
    CHECK(x + gtk_widget_get_allocated_width(widget) <= gtk_widget_get_allocated_width(window));
    CHECK(y + gtk_widget_get_allocated_height(widget) <= gtk_widget_get_allocated_height(window));
}

typedef struct { LdtmApp *app; unsigned int phase; } LayoutProbe;
static gboolean layout_probe(gpointer user_data)
{
    LayoutProbe *probe = user_data;
    LdtmApp *app = probe->app;
    GtkWidget *window = app->window;
    CHECK(GTK_IS_BOX(find_named(window, "ldtm-workspace")));
    CHECK(find_named(window, "ldtm-page-scroll") == NULL);
    GtkWidget *controls[] = { GTK_WIDGET(app->device_combo), app->build_button,
        app->qualify_button, app->verify_button, find_named(window, "ldtm-activity") };
    for (size_t index = 0U; index < G_N_ELEMENTS(controls); ++index)
        check_inside(window, controls[index]);
    if (probe->phase == 0U) {
        const InfiltratrTypography *typography = infiltratr_typography();
        check_font(find_named(window, "ldtm-header-brand-title"), typography->brand_family, 22);
        check_font(find_named(window, "ldtm-hero-title"), typography->brand_family, 26);
        check_font(find_named(window, "ldtm-hero-subtitle"), typography->ui_family, 12);
        check_font(find_named(window, "ldtm-section-title"), typography->ui_family, 16);
        CHECK(gtk_widget_get_sensitive(app->build_button));
        app->worker_running = TRUE; update_interaction_controls(app);
        CHECK(!gtk_widget_get_sensitive(app->build_button));
        CHECK(!gtk_widget_get_sensitive(GTK_WIDGET(app->device_combo)));
        CHECK(gtk_widget_get_sensitive(app->filesystem_tiles[0]));
        app->worker_running = FALSE;
        app->discovering_devices = TRUE; update_interaction_controls(app);
        CHECK(!gtk_widget_get_sensitive(app->qualify_button));
        app->discovering_devices = FALSE;
        GtkTreeIter device;
        CHECK(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->device_store), &device));
        gtk_list_store_set(app->device_store, &device, LDTM_DEVICE_COL_SAFE, FALSE, -1);
        update_interaction_controls(app);
        CHECK(!gtk_widget_get_sensitive(app->verify_button));
        gtk_list_store_set(app->device_store, &device, LDTM_DEVICE_COL_SAFE, TRUE, -1);
        update_interaction_controls(app);
        CHECK(gtk_widget_get_sensitive(app->qualify_button));
        GtkWidget *grid = find_named(window, "ldtm-filesystem-grid");
        GList *tiles = gtk_container_get_children(GTK_CONTAINER(grid));
        CHECK(g_list_length(tiles) == LDTM_SPEC_COUNT);
        g_list_free(tiles);
        capture_window(window, "LDTM_GUI_TEST_DESKTOP_SCREENSHOT");
        gtk_window_resize(GTK_WINDOW(window), 900, 600);
        ++probe->phase; return G_SOURCE_CONTINUE;
    }
    if (probe->phase == 1U) {
        CHECK(gtk_widget_get_allocated_width(window) <= 1024);
        CHECK(gtk_widget_get_allocated_height(window) <= 768);
        GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(find_named(window, "ldtm-grid-scroll")));
        CHECK(gtk_adjustment_get_upper(vertical) > gtk_adjustment_get_page_size(vertical));
        for (size_t index = 0U; index < LDTM_SPEC_COUNT; ++index) {
            CHECK(GTK_IS_BUTTON(app->filesystem_tiles[index]));
            if (strcmp(ldtm_specs()[index].key, "zfs") == 0) gtk_button_clicked(GTK_BUTTON(app->filesystem_tiles[index]));
        }
        CHECK(strstr(gtk_label_get_text(GTK_LABEL(app->selected_title)), "ZFS") != NULL);
        update_filesystem_status(app, "zfs", "qualification-failed", "nlevels=2 indblkshift=11 — retained diagnostic");
        CHECK(strstr(gtk_label_get_text(GTK_LABEL(app->selected_detail)), "retained diagnostic") != NULL);
        CHECK(strstr(gtk_label_get_text(GTK_LABEL(app->result_summary)), "1 failed") != NULL);
        unsigned int dialogs = 0U;
        g_idle_add(close_log_dialog, &dialogs);
        gtk_button_clicked(GTK_BUTTON(find_named(window, "ldtm-filesystem-details")));
        CHECK(dialogs == 1U);

        ++probe->phase; return G_SOURCE_CONTINUE;
    }
    if (probe->phase == 2U) {
        capture_window(window, "LDTM_GUI_TEST_SCREENSHOT");
        gtk_button_clicked(GTK_BUTTON(find_named(window, "ldtm-view-log")));
        CHECK(g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(app->result_stack)), "log") == 0);
        test_final_worker_output(app);
        ++probe->phase; return G_SOURCE_CONTINUE;
    }
    if (probe->phase == 3U) {
        GtkAdjustment *log_scroll = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(find_named(window, "ldtm-log-scroll")));
        CHECK(gtk_adjustment_get_value(log_scroll) + gtk_adjustment_get_page_size(log_scroll) >= gtk_adjustment_get_upper(log_scroll) - 2.0);
        capture_window(window, "LDTM_GUI_TEST_LOG_SCREENSHOT");
        gtk_stack_set_visible_child_name(GTK_STACK(app->result_stack), "details");
        ++probe->phase; return G_SOURCE_CONTINUE;
    }
    gtk_widget_destroy(window);
    ++probe->phase;
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    ldtm_apply_mb_theme();
    check_operation_colours();
    LdtmApp app = {0};
    create_test_media_window(&app);
    reset_filesystem_rows(&app);
    GtkTreeIter device;
    gtk_list_store_append(app.device_store, &device);
    gtk_list_store_set(app.device_store, &device,
        LDTM_DEVICE_COL_PATH, "ui-test-device",
        LDTM_DEVICE_COL_DISPLAY, "USB test disk — 119.1 GiB — ui-test-device",
        LDTM_DEVICE_COL_SAFE, TRUE,
        LDTM_DEVICE_COL_SUMMARY, "USB test disk • 119.1 GiB • USB\nSerial: UI-FIXTURE\nIdentity locked for test fixture", -1);
    gtk_combo_box_set_active(app.device_combo, 0);
    gtk_widget_show_all(app.window);
    LayoutProbe probe = { &app, 0U };
    g_timeout_add(150U, layout_probe, &probe);
    gtk_main();
    CHECK(probe.phase == 5U);
    g_object_unref(app.device_store);
    g_object_unref(app.filesystem_store);
    return 0;
}
