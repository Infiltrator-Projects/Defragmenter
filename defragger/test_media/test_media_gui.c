// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "infiltratr/core.h"

#include <gtk/gtk.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LDTM_DEVICE_COL_PATH 0
#define LDTM_DEVICE_COL_DISPLAY 1
#define LDTM_DEVICE_COL_SAFE 2
#define LDTM_DEVICE_COL_SUMMARY 3
#define LDTM_DEVICE_N_COLUMNS 4

enum {
    LDTM_FS_COL_KEY = 0,
    LDTM_FS_COL_LABEL,
    LDTM_FS_COL_SIZE,
    LDTM_FS_COL_PAYLOAD,
    LDTM_FS_COL_CREATOR,
    LDTM_FS_COL_AVAILABILITY,
    LDTM_FS_COL_RESULT,
    LDTM_FS_COL_DETAIL,
    LDTM_FS_N_COLUMNS
};

typedef struct {
    GtkWidget *window;
    GtkComboBox *device_combo;
    GtkListStore *device_store;
    GtkWidget *device_summary;
    GtkWidget *device_badge;
    GtkWidget *readiness_summary;
    GtkWidget *operation_summary;
    GtkListStore *filesystem_store;
    GtkWidget *filesystem_tiles[LDTM_SPEC_COUNT];
    GtkWidget *filesystem_status[LDTM_SPEC_COUNT];
    GtkWidget *build_button;
    GtkWidget *qualify_button;
    GtkWidget *verify_button;
    GtkWidget *refresh_button;
    GtkWidget *progress;
    GtkWidget *result_stack;
    GtkWidget *result_summary;
    GtkWidget *selected_panel;
    GtkWidget *selected_title;
    GtkWidget *selected_meta;
    GtkWidget *selected_result;
    GtkWidget *selected_detail;
    GtkWidget *activity_spinner;
    size_t selected_filesystem;
    GtkTextBuffer *log_buffer;
    GPid child_pid;
    GIOChannel *stdout_channel;
    GIOChannel *stderr_channel;
    GSubprocess *device_discovery;
    GCancellable *device_discovery_cancel;
    guint stdout_watch;
    guint stderr_watch;
    guint child_watch;
    guint completed_rows;
    gboolean worker_running;
    gboolean discovering_devices;
    gboolean shutting_down;
} LdtmApp;

static void refresh_selected_filesystem(LdtmApp *app);
static void update_result_summary(LdtmApp *app);

static char *pair_value(const char *line, const char *key)
{
    char *needle = g_strdup_printf("%s=\"", key);
    const char *found = strstr(line, needle);
    const char *cursor;
    GString *value;
    g_free(needle);
    if (found == NULL) return g_strdup("");
    cursor = strchr(found, '"');
    if (cursor == NULL) return g_strdup("");
    ++cursor;
    value = g_string_new(NULL);
    while (*cursor != '\0' && *cursor != '"') {
        unsigned char decoded = 0U;
        if (cursor[0] == '\\' && cursor[1] == 'x' &&
            cursor[2] != '\0' && cursor[3] != '\0' &&
            ldtm_decode_hex_byte(cursor[2], cursor[3], &decoded)) {
            g_string_append_c(value, (char)decoded);
            cursor += 4;
        } else if (cursor[0] == '\\' && cursor[1] != '\0') {
            g_string_append_c(value, cursor[1]);
            cursor += 2;
        } else {
            g_string_append_c(value, *cursor++);
        }
    }
    return g_string_free(value, FALSE);
}

static void show_message(GtkWindow *parent, GtkMessageType type,
                         const char *primary, const char *secondary)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        type, GTK_BUTTONS_CLOSE, "%s", primary);
    if (secondary != NULL && *secondary != '\0')
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialog), "%s", secondary);
    (void)gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static gboolean follow_log_end(gpointer data)
{
    GtkTextView *view = GTK_TEXT_VIEW(data);
    g_object_set_data(G_OBJECT(view), "ldtm-follow-source", NULL);
    if (gtk_widget_get_realized(GTK_WIDGET(view))) {
        GtkAdjustment *vertical = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
        gtk_adjustment_set_value(vertical, MAX(gtk_adjustment_get_lower(vertical),
            gtk_adjustment_get_upper(vertical) - gtk_adjustment_get_page_size(vertical)));
    }
    return G_SOURCE_REMOVE;
}

static void append_log(LdtmApp *app, const char *text)
{
    GtkTextIter end;
    GtkTextView *view = GTK_TEXT_VIEW(
        g_object_get_data(G_OBJECT(app->log_buffer), "view"));
    gtk_text_buffer_get_end_iter(app->log_buffer, &end);
    gtk_text_buffer_insert(app->log_buffer, &end, text, -1);
    if (view != NULL) {
        if (g_object_get_data(G_OBJECT(view), "ldtm-follow-source") == NULL) {
            guint source = g_timeout_add_full(G_PRIORITY_LOW, 50U, follow_log_end,
                g_object_ref(view), g_object_unref);
            g_object_set_data(G_OBJECT(view), "ldtm-follow-source", GUINT_TO_POINTER(source));
        }
    }
}

static void copy_operation_log(LdtmApp *app)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(app->log_buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(app->log_buffer, &start, &end, FALSE);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
    g_free(text);
}

static void show_operation_log(LdtmApp *app, const char *title)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        title, GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Copy log", GTK_RESPONSE_APPLY, "Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *view = gtk_text_view_new_with_buffer(app->log_buffer);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 820, 460);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                        scroll, TRUE, TRUE, 8U);
    gtk_widget_show_all(dialog);
    while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_APPLY)
        copy_operation_log(app);
    gtk_widget_destroy(dialog);
}

static void view_log_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    LdtmApp *app = (LdtmApp *)user_data;
    gtk_stack_set_visible_child_name(GTK_STACK(app->result_stack), "log");
    append_log(app, "");
}

static void copy_log_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    copy_operation_log((LdtmApp *)user_data);
}

static const char *display_result(const char *status)
{
    if (status == NULL) return "Unknown";
    if (strcmp(status, "populated") == 0) return "Populated";
    if (strcmp(status, "formatted") == 0) return "Formatted";
    if (strcmp(status, "formatted-unpopulated") == 0) return "Formatted only";
    if (strcmp(status, "skipped") == 0) return "Skipped";
    if (strcmp(status, "format-failed") == 0) return "Format failed";
    if (strcmp(status, "verified") == 0 || strcmp(status, "verify-ok") == 0)
        return "Verified";
    if (strcmp(status, "verify-failed") == 0 || strcmp(status, "verify-fail") == 0)
        return "Verification failed";
    if (strcmp(status, "verify-skip") == 0) return "Verification skipped";
    if (strcmp(status, "qualified") == 0) return "Production qualified";
    if (strcmp(status, "qualification-skipped") == 0) return "Analysis only";
    if (strcmp(status, "qualification-failed") == 0) return "Qualification failed";
    return status;
}

static const char *filesystem_family_class(const char *key)
{
    if (key == NULL) return "ldtm-fs-family-neutral";
    if (g_str_has_prefix(key, "fat") || strcmp(key, "exfat") == 0)
        return "ldtm-fs-family-cyan";
    if (g_str_has_prefix(key, "ext") || strcmp(key, "xfs") == 0 || strcmp(key, "btrfs") == 0)
        return "ldtm-fs-family-green";
    if (strcmp(key, "ntfs") == 0 || strcmp(key, "apfs") == 0 || strcmp(key, "zfs") == 0)
        return "ldtm-fs-family-purple";
    if (strcmp(key, "ofs") == 0 || strcmp(key, "ffs") == 0 ||
        strcmp(key, "sfs") == 0 || strcmp(key, "pfs3") == 0)
        return "ldtm-fs-family-magenta";
    if (strcmp(key, "hfs") == 0 || strcmp(key, "hfsplus") == 0 ||
        strcmp(key, "ufs") == 0 || strcmp(key, "minix") == 0)
        return "ldtm-fs-family-amber";
    return "ldtm-fs-family-neutral";
}

static const char *filesystem_status_class(const char *status)
{
    if (status == NULL || strcmp(status, "Waiting") == 0) return "ldtm-fs-waiting";
    if (strstr(status, "failed") != NULL || strstr(status, "Failed") != NULL)
        return "ldtm-fs-failure";
    if (strstr(status, "only") != NULL || strstr(status, "Skipped") != NULL)
        return "ldtm-fs-warning";
    if (strstr(status, "Populated") != NULL || strstr(status, "Verified") != NULL ||
        strstr(status, "qualified") != NULL || strstr(status, "Formatted") != NULL)
        return "ldtm-fs-success";
    return "ldtm-fs-waiting";
}

static void set_filesystem_tile_status(LdtmApp *app, size_t index,
                                       const char *status, const char *detail)
{
    static const char *classes[] = {
        "ldtm-fs-waiting", "ldtm-fs-success", "ldtm-fs-warning", "ldtm-fs-failure"
    };
    GtkStyleContext *context;
    if (index >= LDTM_SPEC_COUNT || app->filesystem_tiles[index] == NULL ||
        app->filesystem_status[index] == NULL) return;
    gtk_label_set_text(GTK_LABEL(app->filesystem_status[index]), status);
    context = gtk_widget_get_style_context(app->filesystem_tiles[index]);
    for (size_t item = 0U; item < G_N_ELEMENTS(classes); ++item)
        gtk_style_context_remove_class(context, classes[item]);
    gtk_style_context_add_class(context, filesystem_status_class(status));
    refresh_selected_filesystem(app);
    if (detail != NULL && *detail != '\0')
        gtk_widget_set_tooltip_text(app->filesystem_tiles[index], detail);
}

static ssize_t filesystem_index_for_key(const char *key)
{
    const LdtmFilesystemSpec *specs = ldtm_specs();
    for (size_t index = 0U; index < ldtm_spec_count(); ++index)
        if (g_strcmp0(specs[index].key, key) == 0) return (ssize_t)index;
    return -1;
}

static void update_progress_text(LdtmApp *app, const char *filesystem,
                                 const char *result)
{
    char text[192];
    const guint total = (guint)ldtm_spec_count();
    const double fraction = total > 0U ? (double)app->completed_rows / (double)total : 0.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), fraction);
    if (filesystem != NULL && result != NULL)
        (void)snprintf(text, sizeof(text), "%u / %u  •  %s  •  %s",
                       app->completed_rows, total, filesystem, result);
    else
        (void)snprintf(text, sizeof(text), "%u / %u", app->completed_rows, total);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), text);
}

static void update_filesystem_status(LdtmApp *app, const char *key,
                                     const char *status, const char *detail)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->filesystem_store), &iter);
    const char *shown = display_result(status);
    const ssize_t tile_index = filesystem_index_for_key(key);
    while (valid) {
        char *row_key = NULL;
        char *old_result = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->filesystem_store), &iter,
                           LDTM_FS_COL_KEY, &row_key,
                           LDTM_FS_COL_RESULT, &old_result, -1);
        if (g_strcmp0(row_key, key) == 0) {
            if (g_strcmp0(old_result, "Waiting") == 0 && app->completed_rows < ldtm_spec_count())
                ++app->completed_rows;
            gtk_list_store_set(app->filesystem_store, &iter,
                               LDTM_FS_COL_RESULT, shown,
                               LDTM_FS_COL_DETAIL, detail != NULL ? detail : "", -1);
            if (tile_index >= 0)
                set_filesystem_tile_status(app, (size_t)tile_index, shown, detail);
            update_progress_text(app, key, shown);
            update_result_summary(app);
            g_free(old_result);
            g_free(row_key);
            return;
        }
        g_free(old_result);
        g_free(row_key);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->filesystem_store), &iter);
    }
}

static void parse_worker_status(LdtmApp *app, const char *line)
{
    if (g_str_has_prefix(line, "LDTM_STATUS\t")) {
        gchar **fields = g_strsplit(line, "\t", 4);
        if (g_strv_length(fields) >= 4U) {
            g_strchomp(fields[3]);
            update_filesystem_status(app, fields[1], fields[2], fields[3]);
        }
        g_strfreev(fields);
    } else if (g_str_has_prefix(line, "=== ")) {
        const char *start = line + 4;
        const char *end = strstr(start, " ===");
        if (end == NULL) end = strchr(start, '\n');
        if (end != NULL && end > start) {
            char *subject = g_strndup(start, (gsize)(end - start));
            char *message = g_strdup_printf("Working on %s…", subject);
            gtk_label_set_text(GTK_LABEL(app->operation_summary), message);
            gtk_widget_set_tooltip_text(app->operation_summary, message);
            g_free(message);
            g_free(subject);
        }
    }
}

static gboolean channel_watch(GIOChannel *channel, GIOCondition condition, gpointer user_data)
{
    LdtmApp *app = (LdtmApp *)user_data;
    guint lines = 0U;
    if ((condition & (G_IO_IN | G_IO_HUP)) != 0) {
        while (lines < 64U) {
            gchar *line = NULL;
            gsize length = 0U;
            GIOStatus status = g_io_channel_read_line(channel, &line, &length, NULL, NULL);
            if (status == G_IO_STATUS_NORMAL && line != NULL) {
                (void)length;
                append_log(app, line);
                parse_worker_status(app, line);
                g_free(line);
                ++lines;
                continue;
            }
            g_free(line);
            if (status == G_IO_STATUS_AGAIN) return TRUE;
            break;
        }
        if (lines == 64U) return TRUE;
    }
    return (condition & (G_IO_ERR | G_IO_NVAL | G_IO_HUP)) == 0;
}

static void update_interaction_controls(LdtmApp *app)
{
    GtkTreeIter iter;
    gboolean safe = FALSE;
    const gboolean idle = !app->worker_running && !app->discovering_devices;
    if (gtk_combo_box_get_active_iter(app->device_combo, &iter))
        gtk_tree_model_get(GTK_TREE_MODEL(app->device_store), &iter,
                           LDTM_DEVICE_COL_SAFE, &safe, -1);
    gtk_widget_set_sensitive(app->build_button, idle && safe);
    gtk_widget_set_sensitive(app->qualify_button, idle && safe);
    gtk_widget_set_sensitive(app->verify_button, idle && safe);
    gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), idle);
    if (app->refresh_button != NULL) gtk_widget_set_sensitive(app->refresh_button, idle);
}

static void set_worker_controls(LdtmApp *app, gboolean running)
{
    app->worker_running = running;
    update_interaction_controls(app);
    if (app->activity_spinner != NULL) {
        if (running) {
            gtk_widget_show(app->activity_spinner);
            gtk_spinner_start(GTK_SPINNER(app->activity_spinner));
        } else {
            gtk_spinner_stop(GTK_SPINNER(app->activity_spinner));
            gtk_widget_hide(app->activity_spinner);
        }
    }
    if (running) {
        char text[64];
        app->completed_rows = 0U;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
        (void)snprintf(text, sizeof(text), "0 / %zu  •  starting…", ldtm_spec_count());
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), text);
        gtk_label_set_text(GTK_LABEL(app->operation_summary), "Privileged worker starting…");
    } else if (!app->discovering_devices) {
        gtk_label_set_text(GTK_LABEL(app->operation_summary), "Ready");
    }
}

static void cleanup_channels(LdtmApp *app)
{
    if (app->stdout_watch != 0U) { g_source_remove(app->stdout_watch); app->stdout_watch = 0U; }
    if (app->stderr_watch != 0U) { g_source_remove(app->stderr_watch); app->stderr_watch = 0U; }
    if (app->stdout_channel != NULL) { g_io_channel_unref(app->stdout_channel); app->stdout_channel = NULL; }
    if (app->stderr_channel != NULL) { g_io_channel_unref(app->stderr_channel); app->stderr_channel = NULL; }
}

static void drain_finished_channel(LdtmApp *app, GIOChannel *channel)
{
    if (channel == NULL) return;
    for (;;) {
        gchar *line = NULL;
        GIOStatus status = g_io_channel_read_line(channel, &line, NULL, NULL, NULL);
        if (status == G_IO_STATUS_NORMAL && line != NULL) {
            append_log(app, line);
            parse_worker_status(app, line);
            g_free(line);
            continue;
        }
        g_free(line);
        break;
    }
}

static void worker_finished(GPid pid, gint status, gpointer user_data)
{
    LdtmApp *app = (LdtmApp *)user_data;
    char message[128];
    /* The child watch may run before the final pipe-readable notification. */
    drain_finished_channel(app, app->stdout_channel);
    drain_finished_channel(app, app->stderr_channel);
    cleanup_channels(app);
    app->child_watch = 0U;
    app->child_pid = 0;
    g_spawn_close_pid(pid);
    set_worker_controls(app, FALSE);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 1.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "Completed");
        gtk_label_set_text(GTK_LABEL(app->operation_summary), "Operation completed successfully");
        append_log(app, "\nWorker completed successfully.\n");
        return;
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "Operation failed");
    gtk_label_set_text(GTK_LABEL(app->operation_summary), "Operation failed — diagnostic log opened");
    if (app->result_stack != NULL)
        gtk_stack_set_visible_child_name(GTK_STACK(app->result_stack), "log");
    if (WIFEXITED(status))
        (void)snprintf(message, sizeof(message), "\nWorker failed (exit status %d).\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        (void)snprintf(message, sizeof(message), "\nWorker terminated by signal %d.\n", WTERMSIG(status));
    else
        (void)snprintf(message, sizeof(message), "\nWorker failed (wait status %d).\n", status);
    append_log(app, message);
    show_operation_log(app, "Test-media operation failed — diagnostic log");
    if (!app->shutting_down) append_log(app, "");
}

static char *selected_device(LdtmApp *app)
{
    GtkTreeIter iter;
    char *path = NULL;
    if (!gtk_combo_box_get_active_iter(app->device_combo, &iter)) return NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->device_store), &iter,
                       LDTM_DEVICE_COL_PATH, &path, -1);
    return path;
}

static gboolean spawn_worker(LdtmApp *app, const char *operation,
                             const char *device, gboolean confirmed,
                             const char *fingerprint)
{
    gchar *self = g_file_read_link("/proc/self/exe", NULL);
    gchar *argv[10];
    gint stdout_fd = -1, stderr_fd = -1;
    GError *error = NULL;
    guint arg = 0U;
    gboolean started;
    if (self == NULL) self = g_strdup("linux-defragger-test-media");
    argv[arg++] = g_strdup("pkexec");
    argv[arg++] = self;
    argv[arg++] = g_strdup("--worker");
    argv[arg++] = g_strdup(operation);
    argv[arg++] = g_strdup(device);
    if (confirmed) {
        if (fingerprint == NULL || *fingerprint == '\0') {
            for (guint i = 0U; i < arg; ++i) g_free(argv[i]);
            return FALSE;
        }
        argv[arg++] = g_strdup("--confirmed");
        argv[arg++] = g_strdup(device);
        argv[arg++] = g_strdup("--fingerprint");
        argv[arg++] = g_strdup(fingerprint);
    }
    argv[arg] = NULL;
    started = g_spawn_async_with_pipes(
        NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
        NULL, NULL, &app->child_pid, NULL, &stdout_fd, &stderr_fd, &error);
    for (guint i = 0U; i < arg; ++i) g_free(argv[i]);
    if (!started) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                     "Could not start privileged worker",
                     error != NULL ? error->message : "Unknown process-launch error");
        g_clear_error(&error);
        return FALSE;
    }
    app->stdout_channel = g_io_channel_unix_new(stdout_fd);
    app->stderr_channel = g_io_channel_unix_new(stderr_fd);
    g_io_channel_set_close_on_unref(app->stdout_channel, TRUE);
    g_io_channel_set_close_on_unref(app->stderr_channel, TRUE);
    (void)g_io_channel_set_encoding(app->stdout_channel, NULL, NULL);
    (void)g_io_channel_set_encoding(app->stderr_channel, NULL, NULL);
    (void)g_io_channel_set_flags(app->stdout_channel,
        g_io_channel_get_flags(app->stdout_channel) | G_IO_FLAG_NONBLOCK, NULL);
    (void)g_io_channel_set_flags(app->stderr_channel,
        g_io_channel_get_flags(app->stderr_channel) | G_IO_FLAG_NONBLOCK, NULL);
    app->stdout_watch = g_io_add_watch(app->stdout_channel,
        G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, channel_watch, app);
    app->stderr_watch = g_io_add_watch(app->stderr_channel,
        G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, channel_watch, app);
    app->child_watch = g_child_watch_add(app->child_pid, worker_finished, app);
    set_worker_controls(app, TRUE);
    return TRUE;
}

static GtkWidget *make_icon_well(const char *icon_name, const char *css_class, gint pixel_size)
{
    GtkWidget *well = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    GtkStyleContext *style = gtk_widget_get_style_context(well);
    gtk_style_context_add_class(style, "ldtm-icon-well");
    if (css_class != NULL) gtk_style_context_add_class(style, css_class);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), pixel_size);
    gtk_widget_set_halign(well, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(well, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(well), icon, FALSE, FALSE, 0);
    return well;
}

static gboolean confirmation_dialog(LdtmApp *app, const char *device)
{
    GtkWidget *dialog, *content, *panel, *box, *label, *entry, *accept;
    char *expected = g_strdup_printf("DESTROY %s", device);
    gboolean accepted = FALSE;
    gint response;
    dialog = gtk_dialog_new_with_buttons(
        "Destroy and build test disk", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Destroy and Build", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 640, -1);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    panel = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(panel), GTK_SHADOW_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "ldtm-danger-panel");
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16U);
    gtk_container_add(GTK_CONTAINER(panel), box);
    gtk_box_pack_start(GTK_BOX(box), make_icon_well("edit-delete-symbolic", "ldtm-danger-icon-well", 32), FALSE, FALSE, 0);
    label = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    {
        char *markup = g_markup_printf_escaped(
            "<span size=\"x-large\" weight=\"bold\">Erase the entire disk %s?</span>\n\n"
            "Every existing partition and file on this device will be destroyed. "
            "The privileged worker repeats the complete identity and safety check before writing.\n\n"
            "Type the exact confirmation below:\n<b>%s</b>", device, expected);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
    }
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), expected);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), panel, TRUE, TRUE, 12U);
    accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    if (accept != NULL)
        gtk_style_context_add_class(gtk_widget_get_style_context(accept), "ldtm-destructive-action");
    gtk_widget_show_all(dialog);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT &&
        g_strcmp0(gtk_entry_get_text(GTK_ENTRY(entry)), expected) == 0)
        accepted = TRUE;
    else if (response == GTK_RESPONSE_ACCEPT)
        show_message(GTK_WINDOW(dialog), GTK_MESSAGE_ERROR,
                     "Confirmation did not match", "No disk changes were made.");
    gtk_widget_destroy(dialog);
    g_free(expected);
    return accepted;
}

static void reset_filesystem_rows(LdtmApp *app)
{
    guint ready = 0U, package_missing = 0U, unavailable = 0U, manual = 0U;
    char summary[64];
    gtk_list_store_clear(app->filesystem_store);
    for (size_t index = 0U; index < ldtm_spec_count(); ++index) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
        GtkTreeIter iter;
        char size[32], payload[32], creator_detail[256], detail[512];
        const gboolean available = ldtm_spec_creator_available(
            spec, creator_detail, sizeof(creator_detail)) != 0;
        const char *availability;
        (void)snprintf(size, sizeof(size), "%u MiB", spec->size_mib);
        if (spec->payload_mib == 0U) (void)snprintf(payload, sizeof(payload), "—");
        else (void)snprintf(payload, sizeof(payload), "%u MiB", spec->payload_mib);
        if (spec->creator == LDTM_CREATOR_MANUAL) {
            availability = "Reserved / manual"; ++manual;
            (void)snprintf(detail, sizeof(detail), "%s", spec->note);
        } else if (available) {
            availability = "Ready"; ++ready;
            (void)snprintf(detail, sizeof(detail), "%s", creator_detail);
        } else if (spec->package_hint != NULL && *spec->package_hint != '\0') {
            availability = "Optional package missing"; ++package_missing;
            (void)snprintf(detail, sizeof(detail), "%s. Install package '%s' to enable this test slot.",
                           creator_detail, spec->package_hint);
        } else {
            availability = "No standard creator"; ++unavailable;
            (void)snprintf(detail, sizeof(detail),
                           "No supported creator is installed for this format. The slot is reserved and the rest of the build continues.%s%s",
                           (spec->note != NULL && *spec->note != '\0') ? " " : "",
                           spec->note != NULL ? spec->note : "");
        }
        gtk_list_store_append(app->filesystem_store, &iter);
        gtk_list_store_set(app->filesystem_store, &iter,
                           LDTM_FS_COL_KEY, spec->key,
                           LDTM_FS_COL_LABEL, spec->label,
                           LDTM_FS_COL_SIZE, size,
                           LDTM_FS_COL_PAYLOAD, payload,
                           LDTM_FS_COL_CREATOR, ldtm_creator_display_name(spec),
                           LDTM_FS_COL_AVAILABILITY, availability,
                           LDTM_FS_COL_RESULT, "Waiting",
                           LDTM_FS_COL_DETAIL, detail, -1);
        if (index < LDTM_SPEC_COUNT) {
            char tile_detail[768];
            (void)snprintf(tile_detail, sizeof(tile_detail), "%s  •  %s  •  %s\n%s",
                           spec->label, size, ldtm_creator_display_name(spec), detail);
            set_filesystem_tile_status(app, index, "Waiting", tile_detail);
        }
    }
    (void)snprintf(summary, sizeof(summary), "%u / %zu", ready, ldtm_spec_count());
    gtk_label_set_text(GTK_LABEL(app->readiness_summary), summary);
    {
        char *tip = g_strdup_printf("%u ready • %u optional package missing • %u unavailable • %u reserved",
                                    ready, package_missing, unavailable, manual);
        gtk_widget_set_tooltip_text(app->readiness_summary, tip);
        g_free(tip);
    }
    app->completed_rows = 0U;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "Ready");
    refresh_selected_filesystem(app);
    update_result_summary(app);
}

static void device_changed(GtkComboBox *combo, gpointer user_data)
{
    LdtmApp *app = (LdtmApp *)user_data;
    GtkTreeIter iter;
    char *summary = NULL;
    gboolean safe = FALSE;
    GtkStyleContext *style;
    (void)combo;
    if (gtk_combo_box_get_active_iter(app->device_combo, &iter))
        gtk_tree_model_get(GTK_TREE_MODEL(app->device_store), &iter,
                           LDTM_DEVICE_COL_SUMMARY, &summary,
                           LDTM_DEVICE_COL_SAFE, &safe, -1);
    if (summary != NULL) {
        gchar **lines = g_strsplit(summary, "\n", 3);
        const char *identity = g_strv_length(lines) > 1U ? lines[1] : lines[0];
        char *compact = g_strdup_printf("%s  •  %s",
            safe ? "Identity locked" : "Protected disk — operations disabled", identity);
        gtk_label_set_text(GTK_LABEL(app->device_summary), compact);
        gtk_widget_set_tooltip_text(app->device_summary, summary);
        g_free(compact);
        g_strfreev(lines);
    } else gtk_label_set_text(GTK_LABEL(app->device_summary), "Choose a physical test disk.");
    style = gtk_widget_get_style_context(app->device_badge);
    gtk_style_context_remove_class(style, "ldtm-badge-safe");
    gtk_style_context_remove_class(style, "ldtm-badge-protected");
    if (safe) {
        gtk_label_set_text(GTK_LABEL(app->device_badge), "●  Identity locked");
        gtk_style_context_add_class(style, "ldtm-badge-safe");
    } else {
        gtk_label_set_text(GTK_LABEL(app->device_badge), "●  Protected");
        gtk_style_context_add_class(style, "ldtm-badge-protected");
    }
    g_free(summary);
    update_interaction_controls(app);
    reset_filesystem_rows(app);
}

static void populate_devices(LdtmApp *app, const gchar *stdout_text)
{
    gchar **lines = g_strsplit(stdout_text != NULL ? stdout_text : "", "\n", -1);
    gint first_safe = -1, row = 0;
    gtk_list_store_clear(app->device_store);
    for (guint index = 0U; lines[index] != NULL; ++index) {
        char *path = NULL, *size_text = NULL, *model = NULL, *serial = NULL;
        char *wwn = NULL, *transport = NULL, *rm_text = NULL, *ro_text = NULL;
        uint64_t bytes = 0U, removable_value = 0U, readonly_value = 0U;
        gboolean system_disk, field_media, enough, stable_identity, safe;
        char *display = NULL, *summary = NULL;
        GtkTreeIter iter;
        if (*lines[index] == '\0') continue;
        path = pair_value(lines[index], "PATH");
        size_text = pair_value(lines[index], "SIZE");
        model = pair_value(lines[index], "MODEL");
        serial = pair_value(lines[index], "SERIAL");
        wwn = pair_value(lines[index], "WWN");
        transport = pair_value(lines[index], "TRAN");
        rm_text = pair_value(lines[index], "RM");
        ro_text = pair_value(lines[index], "RO");
        if (!infiltratr_parse_u64(size_text, 10U, &bytes) ||
            !infiltratr_parse_u64_range(rm_text, 10U, 0U, 1U, &removable_value) ||
            !infiltratr_parse_u64_range(ro_text, 10U, 0U, 1U, &readonly_value)) goto cleanup_line;
        system_disk = ldtm_is_system_disk(path) != 0;
        field_media = ldtm_transport_is_field_media((int)removable_value, transport) != 0;
        enough = bytes >= ldtm_required_capacity_bytes();
        stable_identity = *serial != '\0' || *wwn != '\0';
        safe = !system_disk && readonly_value == 0U && field_media && enough && stable_identity;
        display = g_strdup_printf("%s   %.1f GiB   %s%s", path,
                                  (double)bytes / (double)LDTM_GIB,
                                  *model != '\0' ? model : "unknown model",
                                  system_disk ? "   PROTECTED SYSTEM DISK" :
                                  (safe ? "   field-media candidate" : ""));
        summary = g_strdup_printf("%s  •  %.1f GiB  •  %s\nSerial %s   WWN %s\n%s",
            *model != '\0' ? model : "Unknown device",
            (double)bytes / (double)LDTM_GIB,
            *transport != '\0' ? transport : "unknown transport",
            *serial != '\0' ? serial : "unknown",
            *wwn != '\0' ? wwn : "unknown",
            system_disk ? "Protected system storage — destructive work is disabled." :
            (!field_media ? "Not accepted as removable/USB/MMC field media." :
             (!enough ? "Too small for the complete 21-partition qualification layout." :
              (!stable_identity ? "No stable identity is available for destructive confirmation." :
               "Identity locked. The privileged worker re-verifies this exact disk before writing."))));
        gtk_list_store_append(app->device_store, &iter);
        gtk_list_store_set(app->device_store, &iter,
                           LDTM_DEVICE_COL_PATH, path,
                           LDTM_DEVICE_COL_DISPLAY, display,
                           LDTM_DEVICE_COL_SAFE, safe,
                           LDTM_DEVICE_COL_SUMMARY, summary, -1);
        if (safe && first_safe < 0) first_safe = row;
        ++row;
cleanup_line:
        g_free(summary); g_free(display); g_free(ro_text); g_free(rm_text);
        g_free(transport); g_free(wwn); g_free(serial); g_free(model);
        g_free(size_text); g_free(path);
    }
    g_strfreev(lines);
    if (first_safe >= 0) gtk_combo_box_set_active(app->device_combo, first_safe);
    else if (row > 0) gtk_combo_box_set_active(app->device_combo, 0);
    else gtk_label_set_text(GTK_LABEL(app->device_summary), "No physical disks found.");
}

static void refresh_devices_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    LdtmApp *app = (LdtmApp *)user_data;
    gchar *stdout_text = NULL, *stderr_text = NULL;
    GError *error = NULL;
    const gboolean received = g_subprocess_communicate_utf8_finish(
        G_SUBPROCESS(source), result, &stdout_text, &stderr_text, &error);
    const gboolean exited = received && g_subprocess_get_if_exited(G_SUBPROCESS(source));
    const gint exit_status = exited ? g_subprocess_get_exit_status(G_SUBPROCESS(source)) : 127;
    if (!app->shutting_down) {
        if (!received || exit_status != 0) {
            const char *detail = error != NULL ? error->message :
                (stderr_text != NULL && *stderr_text != '\0' ? stderr_text : "lsblk did not complete successfully");
            show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                         "Could not enumerate physical disks", detail);
        } else populate_devices(app, stdout_text);
    }
    g_clear_error(&error); g_free(stdout_text); g_free(stderr_text);
    if (app->device_discovery != NULL) { g_object_unref(app->device_discovery); app->device_discovery = NULL; }
    if (app->device_discovery_cancel != NULL) { g_object_unref(app->device_discovery_cancel); app->device_discovery_cancel = NULL; }
    app->discovering_devices = FALSE;
    if (!app->shutting_down) {
        gtk_label_set_text(GTK_LABEL(app->operation_summary), "Ready");
        update_interaction_controls(app);
    }
}

static void refresh_devices(LdtmApp *app)
{
    const gchar *argv[] = { "lsblk", "-d", "-b", "-n", "-P", "-o",
                            "PATH,SIZE,MODEL,SERIAL,WWN,TRAN,RM,RO", NULL };
    GError *error = NULL;
    if (app->worker_running || app->discovering_devices || app->shutting_down) return;
    app->device_discovery = g_subprocess_newv(
        argv, (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
    if (app->device_discovery == NULL) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                     "Could not enumerate physical disks",
                     error != NULL ? error->message : "Could not start lsblk");
        g_clear_error(&error);
        return;
    }
    app->device_discovery_cancel = g_cancellable_new();
    app->discovering_devices = TRUE;
    gtk_list_store_clear(app->device_store);
    gtk_label_set_text(GTK_LABEL(app->device_summary), "Discovering physical disks…");
    gtk_label_set_text(GTK_LABEL(app->device_badge), "●  Scanning…");
    gtk_label_set_text(GTK_LABEL(app->operation_summary), "Refreshing physical disk list…");
    update_interaction_controls(app);
    g_subprocess_communicate_utf8_async(app->device_discovery, NULL,
        app->device_discovery_cancel, refresh_devices_finished, app);
}

static void refresh_clicked(GtkButton *button, gpointer user_data)
{ (void)button; refresh_devices((LdtmApp *)user_data); }

static void prepare_results_for_operation(LdtmApp *app)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->filesystem_store), &iter);
    size_t index = 0U;
    while (valid) {
        gtk_list_store_set(app->filesystem_store, &iter, LDTM_FS_COL_RESULT, "Waiting", -1);
        if (index < ldtm_spec_count()) set_filesystem_tile_status(app, index, "Waiting", NULL);
        ++index;
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->filesystem_store), &iter);
    }
    app->completed_rows = 0U;
    update_result_summary(app);
}

static void build_clicked(GtkButton *button, gpointer user_data)
{
    LdtmApp *app = (LdtmApp *)user_data;
    char *device = selected_device(app);
    char fingerprint[65];
    (void)button;
    if (device == NULL) return;
    if (ldtm_device_fingerprint(device, fingerprint) != 0) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                     "Could not bind the selected physical disk",
                     "The disk identity could not be read. No disk changes were made.");
        g_free(device); return;
    }
    if (confirmation_dialog(app, device)) {
        gtk_text_buffer_set_text(app->log_buffer, "", -1);
        prepare_results_for_operation(app);
        append_log(app, "Preparing destructive filesystem test media...\n");
        (void)spawn_worker(app, "prepare", device, TRUE, fingerprint);
    }
    g_free(device);
}

static void qualify_clicked(GtkButton *button, gpointer user_data)
{
    LdtmApp *app = (LdtmApp *)user_data;
    char *device = selected_device(app);
    (void)button;
    if (device == NULL) return;
    gtk_text_buffer_set_text(app->log_buffer, "", -1);
    prepare_results_for_operation(app);
    append_log(app, "Running production Defragment and Growth Defrag on every writable test partition, then verifying all retained payload bytes...\n");
    (void)spawn_worker(app, "qualify", device, FALSE, NULL);
    g_free(device);
}

static void verify_clicked(GtkButton *button, gpointer user_data)
{
    LdtmApp *app = (LdtmApp *)user_data;
    char *device = selected_device(app);
    (void)button;
    if (device == NULL) return;
    gtk_text_buffer_set_text(app->log_buffer, "", -1);
    prepare_results_for_operation(app);
    append_log(app, "Verifying retained test payloads against the C-generated manifest...\n");
    (void)spawn_worker(app, "verify", device, FALSE, NULL);
    g_free(device);
}

static GtkTreeViewColumn *append_text_column(GtkTreeView *view, const char *title,
                                             int model_column, gint min_width, gboolean expand)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
        title, renderer, "text", model_column, NULL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_min_width(column, min_width);
    gtk_tree_view_column_set_expand(column, expand);
    gtk_tree_view_append_column(view, column);
    return column;
}

static GtkWidget *make_tree_view(LdtmApp *app)
{
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->filesystem_store));
    GtkTreeView *tree = GTK_TREE_VIEW(view);
    (void)append_text_column(tree, "Filesystem", LDTM_FS_COL_KEY, 90, FALSE);
    (void)append_text_column(tree, "Partition", LDTM_FS_COL_LABEL, 120, FALSE);
    (void)append_text_column(tree, "Size", LDTM_FS_COL_SIZE, 90, FALSE);
    (void)append_text_column(tree, "Payload", LDTM_FS_COL_PAYLOAD, 90, FALSE);
    (void)append_text_column(tree, "Creator", LDTM_FS_COL_CREATOR, 130, TRUE);
    (void)append_text_column(tree, "Availability", LDTM_FS_COL_AVAILABILITY, 180, TRUE);
    (void)append_text_column(tree, "Result", LDTM_FS_COL_RESULT, 190, TRUE);
    gtk_tree_view_set_headers_visible(tree, TRUE);
    gtk_tree_view_set_grid_lines(tree, GTK_TREE_VIEW_GRID_LINES_NONE);
    gtk_tree_view_set_enable_search(tree, TRUE);
    gtk_tree_view_set_search_column(tree, LDTM_FS_COL_KEY);
    gtk_tree_view_set_tooltip_column(tree, LDTM_FS_COL_DETAIL);
    return view;
}

static GtkWidget *make_device_combo(LdtmApp *app)
{
    GtkWidget *combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(app->device_store));
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), renderer,
                                   "text", LDTM_DEVICE_COL_DISPLAY, NULL);
    return combo;
}

static GtkWidget *make_operation_button(const char *icon_name, const char *title,
                                        const char *subtitle, const char *accent_class)
{
    GtkWidget *button = gtk_button_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *title_label = gtk_label_new(title);
    GtkWidget *subtitle_label = gtk_label_new(subtitle);
    GtkStyleContext *style = gtk_widget_get_style_context(button);
    gtk_style_context_add_class(style, "ldtm-operation-button");
    if (accent_class != NULL) gtk_style_context_add_class(style, accent_class);
    gtk_container_set_border_width(GTK_CONTAINER(row), 8U);
    gtk_container_add(GTK_CONTAINER(button), row);
    gtk_box_pack_start(GTK_BOX(row), make_icon_well(icon_name, accent_class, 24), FALSE, FALSE, 0);
    gtk_widget_set_halign(title_label, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(title_label), "ldtm-operation-title");
    gtk_widget_set_halign(subtitle_label, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(subtitle_label), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(subtitle_label), "ldtm-operation-subtitle");
    gtk_box_pack_start(GTK_BOX(copy), title_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(copy), subtitle_label, FALSE, FALSE, 0);
    gtk_label_set_line_wrap(GTK_LABEL(title_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(title_label), 28);
    gtk_label_set_max_width_chars(GTK_LABEL(subtitle_label), 34);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0F);
    gtk_label_set_xalign(GTK_LABEL(subtitle_label), 0.0F);
    gtk_box_pack_start(GTK_BOX(row), copy, TRUE, TRUE, 0);
    return button;
}

static void update_result_summary(LdtmApp *app)
{
    if (app->result_summary == NULL) return;
    GtkTreeIter iter;
    guint complete = 0U, failed = 0U, waiting = 0U;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->filesystem_store), &iter);
    while (valid) {
        char *result = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->filesystem_store), &iter, LDTM_FS_COL_RESULT, &result, -1);
        const char *state = filesystem_status_class(result);
        if (strcmp(state, "ldtm-fs-failure") == 0) ++failed;
        else if (strcmp(state, "ldtm-fs-waiting") == 0) ++waiting;
        else ++complete;
        g_free(result);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->filesystem_store), &iter);
    }
    char *text = g_strdup_printf("%u complete  •  %u failed  •  %u waiting", complete, failed, waiting);
    gtk_label_set_text(GTK_LABEL(app->result_summary), text);
    g_free(text);
}

static void refresh_selected_filesystem(LdtmApp *app)
{
    if (app->selected_panel == NULL || app->selected_filesystem >= ldtm_spec_count()) return;
    GtkTreeIter iter;
    if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(app->filesystem_store), &iter, NULL,
                                      (gint)app->selected_filesystem)) return;
    char *key = NULL, *partition = NULL, *size = NULL, *creator = NULL, *result = NULL, *detail = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->filesystem_store), &iter,
        LDTM_FS_COL_KEY, &key, LDTM_FS_COL_LABEL, &partition, LDTM_FS_COL_SIZE, &size,
        LDTM_FS_COL_CREATOR, &creator, LDTM_FS_COL_RESULT, &result, LDTM_FS_COL_DETAIL, &detail, -1);
    char *title = g_ascii_strup(key, -1);
    char *meta = g_strdup_printf("%s  •  %s  •  %s", partition, size, creator);
    gtk_label_set_text(GTK_LABEL(app->selected_title), title);
    gtk_label_set_text(GTK_LABEL(app->selected_meta), meta);
    gtk_label_set_text(GTK_LABEL(app->selected_result), result);
    gtk_label_set_text(GTK_LABEL(app->selected_detail), detail != NULL && *detail != '\0' ? detail : "No diagnostic recorded.");
    gtk_widget_set_tooltip_text(app->selected_detail, detail);
    GtkStyleContext *style = gtk_widget_get_style_context(app->selected_panel);
    const char *states[] = { "ldtm-fs-waiting", "ldtm-fs-success", "ldtm-fs-warning", "ldtm-fs-failure" };
    for (size_t index = 0U; index < G_N_ELEMENTS(states); ++index)
        gtk_style_context_remove_class(style, states[index]);
    gtk_style_context_add_class(style, filesystem_status_class(result));
    g_free(title); g_free(meta); g_free(key); g_free(partition); g_free(size);
    g_free(creator); g_free(result); g_free(detail);
}

static void filesystem_clicked(GtkButton *button, gpointer user_data)
{
    LdtmApp *app = user_data;
    app->selected_filesystem = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "ldtm-index"));
    for (size_t index = 0U; index < ldtm_spec_count(); ++index) {
        GtkStyleContext *style = gtk_widget_get_style_context(app->filesystem_tiles[index]);
        if (index == app->selected_filesystem) gtk_style_context_add_class(style, "ldtm-fs-selected");
        else gtk_style_context_remove_class(style, "ldtm-fs-selected");
    }
    refresh_selected_filesystem(app);
}

static void filesystem_details_clicked(GtkButton *button, gpointer user_data)
{
    LdtmApp *app = user_data;
    (void)button;
    GtkTreeIter iter;
    if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(app->filesystem_store), &iter, NULL,
                                      (gint)app->selected_filesystem)) return;
    char *values[LDTM_FS_N_COLUMNS] = {0};
    const char *names[] = { "Filesystem", "Partition", "Size", "Payload", "Creator", "Availability", "Result", "Diagnostic" };
    GString *text = g_string_new(NULL);
    for (gint index = 0; index < LDTM_FS_N_COLUMNS; ++index) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->filesystem_store), &iter, index, &values[index], -1);
        g_string_append_printf(text, "%s: %s\n", names[index], values[index] != NULL ? values[index] : "");
    }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Filesystem details", GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Copy details", GTK_RESPONSE_APPLY, "Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *view = gtk_text_view_new();
    gtk_window_set_default_size(GTK_WINDOW(dialog), 720, 380);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), text->str, -1);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll, TRUE, TRUE, 8U);
    gtk_widget_show_all(dialog);
    while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_APPLY)
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text->str, -1);
    gtk_widget_destroy(dialog);
    for (gint index = 0; index < LDTM_FS_N_COLUMNS; ++index) g_free(values[index]);
    g_string_free(text, TRUE);
}

static void disk_details_clicked(GtkButton *button, gpointer user_data)
{
    LdtmApp *app = user_data;
    (void)button;
    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(app->device_combo, &iter)) return;
    char *display = NULL, *summary = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->device_store), &iter,
        LDTM_DEVICE_COL_DISPLAY, &display, LDTM_DEVICE_COL_SUMMARY, &summary, -1);
    show_message(GTK_WINDOW(app->window), GTK_MESSAGE_INFO, display, summary);
    g_free(display); g_free(summary);
}

static GtkWidget *make_filesystem_tile(LdtmApp *app, size_t index)
{
    const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
    GtkWidget *button = gtk_button_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    char *title = g_ascii_strup(spec->key, -1);
    GtkWidget *name = gtk_label_new(title), *size = gtk_label_new(NULL), *status = gtk_label_new("Waiting");
    g_free(title);
    char size_text[64];
    GtkStyleContext *style = gtk_widget_get_style_context(button);
    gtk_style_context_add_class(style, "ldtm-fs-tile");
    gtk_style_context_add_class(style, filesystem_family_class(spec->key));
    gtk_style_context_add_class(style, "ldtm-fs-waiting");
    if (index == app->selected_filesystem) gtk_style_context_add_class(style, "ldtm-fs-selected");
    gtk_container_set_border_width(GTK_CONTAINER(box), 8U);
    gtk_container_add(GTK_CONTAINER(button), box);
    gtk_widget_set_halign(name, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(name), "ldtm-fs-name");
    gtk_box_pack_start(GTK_BOX(top), name, TRUE, TRUE, 0);
    (void)snprintf(size_text, sizeof(size_text), "%u MiB", spec->size_mib);
    gtk_label_set_text(GTK_LABEL(size), size_text);
    gtk_widget_set_halign(size, GTK_ALIGN_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(size), "ldtm-fs-size");
    gtk_box_pack_end(GTK_BOX(top), size, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(status), 18);
    gtk_label_set_xalign(GTK_LABEL(status), 0.0F);
    gtk_style_context_add_class(gtk_widget_get_style_context(status), "ldtm-fs-status");
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(button), "ldtm-index", GUINT_TO_POINTER((guint)index));
    g_signal_connect(button, "clicked", G_CALLBACK(filesystem_clicked), app);
    app->filesystem_tiles[index] = button;
    app->filesystem_status[index] = status;
    return button;
}

static void minimize_window(GtkButton *button, gpointer user_data)
{ (void)button; gtk_window_iconify(GTK_WINDOW(user_data)); }

static void toggle_maximize_window(GtkButton *button, gpointer user_data)
{
    GtkWindow *window = GTK_WINDOW(user_data);
    (void)button;
    if (gtk_window_is_maximized(window)) gtk_window_unmaximize(window);
    else gtk_window_maximize(window);
}

static void close_window(GtkButton *button, gpointer user_data)
{ (void)button; gtk_window_close(GTK_WINDOW(user_data)); }

static GtkWidget *make_suite_header(LdtmApp *app)
{
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *title = gtk_label_new("Defragmenter Test Media");
    GtkWidget *subtitle = gtk_label_new("Infiltrator OS");
    GtkWidget *end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *minimize = gtk_button_new_from_icon_name("window-minimize-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *maximize = gtk_button_new_from_icon_name("window-maximize-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *empty_title = gtk_label_new("");
    GtkWidget *brand_icon = make_icon_well("io.github.linuxdefragger", "ldtm-accent-cyan", 27);
    GtkWidget *controls[] = { minimize, maximize, close };
    gtk_widget_set_name(header, "ldtm-shell-header");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), FALSE);
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header), empty_title);
    gtk_box_pack_start(GTK_BOX(brand), brand_icon, FALSE, FALSE, 0);
    gtk_widget_set_name(title, "ldtm-header-brand-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_set_name(subtitle, "ldtm-header-brand-subtitle");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(copy), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(copy), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brand), copy, FALSE, FALSE, 0);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), brand);
    for (size_t i = 0U; i < G_N_ELEMENTS(controls); ++i)
        gtk_style_context_add_class(gtk_widget_get_style_context(controls[i]), "ldtm-window-control");
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "ldtm-window-control-close");
    gtk_widget_set_tooltip_text(minimize, "Minimize");
    gtk_widget_set_tooltip_text(maximize, "Maximize / Restore");
    gtk_widget_set_tooltip_text(close, "Close");
    g_signal_connect(minimize, "clicked", G_CALLBACK(minimize_window), app->window);
    g_signal_connect(maximize, "clicked", G_CALLBACK(toggle_maximize_window), app->window);
    g_signal_connect(close, "clicked", G_CALLBACK(close_window), app->window);
    gtk_box_pack_start(GTK_BOX(end), minimize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(end), maximize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(end), close, FALSE, FALSE, 0);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), end);
    return header;
}

static void window_destroyed(GtkWidget *widget, gpointer user_data)
{
    LdtmApp *app = (LdtmApp *)user_data;
    (void)widget;
    app->shutting_down = TRUE;
    if (app->device_discovery_cancel != NULL) g_cancellable_cancel(app->device_discovery_cancel);
    if (app->device_discovery != NULL) g_subprocess_force_exit(app->device_discovery);
    if (app->child_pid != 0) (void)kill(app->child_pid, SIGTERM);
    gtk_main_quit();
}

static GtkWidget *make_label(const char *text, const char *name)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    if (name != NULL) gtk_widget_set_name(label, name);
    return label;
}

static GtkWidget *make_panel(const char *css_class, GtkWidget *child)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "ldtm-card");
    if (css_class != NULL) gtk_style_context_add_class(gtk_widget_get_style_context(frame), css_class);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

static GtkWidget *make_scroll(GtkWidget *child, const char *name)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(scroll, name);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(scroll), child);
    return scroll;
}

static void create_test_media_window(LdtmApp *app)
{
    app->device_store = gtk_list_store_new(LDTM_DEVICE_N_COLUMNS,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING);
    app->filesystem_store = gtk_list_store_new(LDTM_FS_N_COLUMNS,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "Defragmenter Test Media");
    GtkWidget *header = make_suite_header(app);
    gtk_window_set_titlebar(GTK_WINDOW(app->window), header);
    gtk_widget_show_all(header);
    gint header_height = 0;
    gtk_widget_get_preferred_height(header, NULL, &header_height);
    GdkRectangle workarea = {0, 0, 1280, 800};
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (monitor == NULL) monitor = gdk_display_get_monitor(display, 0);
    if (monitor != NULL) gdk_monitor_get_workarea(monitor, &workarea);
    const gint available_width = MAX(1, workarea.width - 16);
    const gint available_height = MAX(1, workarea.height - header_height - 16);
    gtk_window_set_default_size(GTK_WINDOW(app->window), MIN(1280, available_width), MIN(760, available_height));
    gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);
    GdkGeometry geometry = {0};
    geometry.min_width = MIN(900, available_width);
    geometry.min_height = MIN(600, available_height);
    gtk_window_set_geometry_hints(GTK_WINDOW(app->window), app->window, &geometry, GDK_HINT_MIN_SIZE);
    g_signal_connect(app->window, "destroy", G_CALLBACK(window_destroyed), app);

    /* The shell stays in the work area; only result content scrolls. */
    GtkWidget *shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_name(shell, "ldtm-workspace");
    gtk_container_set_border_width(GTK_CONTAINER(shell), 12U);
    gtk_container_add(GTK_CONTAINER(app->window), shell);

    GtkWidget *banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(banner), 10U);
    GtkWidget *hero = make_panel(NULL, banner);
    gtk_widget_set_name(hero, "ldtm-hero");
    gtk_box_pack_start(GTK_BOX(banner), make_icon_well("drive-harddisk-symbolic", "ldtm-hero-icon", 28), FALSE, FALSE, 0);
    GtkWidget *banner_copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(banner_copy), make_label("Disk qualification", "ldtm-hero-title"), FALSE, FALSE, 0);
    GtkWidget *intro = make_label("Build fragmented test media. Qualify the engines. Verify the files.", "ldtm-hero-subtitle");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 52);
    gtk_box_pack_start(GTK_BOX(banner_copy), intro, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(banner), banner_copy, TRUE, TRUE, 0);
    GtkWidget *facts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(facts, GTK_ALIGN_CENTER);
    GtkWidget *ready = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(ready), "ldtm-ready-badge");
    gtk_box_pack_start(GTK_BOX(ready), make_label("Creators ready", NULL), FALSE, FALSE, 0);
    app->readiness_summary = make_label("0 / 21", "ldtm-ready-count");
    gtk_box_pack_end(GTK_BOX(ready), app->readiness_summary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(facts), ready, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(facts), make_label("21 layouts  •  200 MiB per data slot", "ldtm-layout-facts"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(banner), facts, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(shell), hero, FALSE, FALSE, 0);

    GtkWidget *target = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(target), 9U);
    GtkWidget *target_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(target_row), make_icon_well("drive-removable-media-symbolic", "ldtm-accent-cyan", 22), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_row), make_label("Test disk", "ldtm-target-title"), FALSE, FALSE, 0);
    app->device_combo = GTK_COMBO_BOX(make_device_combo(app));
    gtk_widget_set_name(GTK_WIDGET(app->device_combo), "ldtm-disk-selector");
    g_signal_connect(app->device_combo, "changed", G_CALLBACK(device_changed), app);
    gtk_box_pack_start(GTK_BOX(target_row), GTK_WIDGET(app->device_combo), TRUE, TRUE, 0);
    app->refresh_button = gtk_button_new_with_label("Refresh");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->refresh_button), "ldtm-tool-button");
    g_signal_connect(app->refresh_button, "clicked", G_CALLBACK(refresh_clicked), app);
    gtk_box_pack_start(GTK_BOX(target_row), app->refresh_button, FALSE, FALSE, 0);
    GtkWidget *disk_details = gtk_button_new_from_icon_name("dialog-information-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(disk_details, "Disk identity and safety details");
    gtk_style_context_add_class(gtk_widget_get_style_context(disk_details), "ldtm-tool-button");
    g_signal_connect(disk_details, "clicked", G_CALLBACK(disk_details_clicked), app);
    gtk_box_pack_start(GTK_BOX(target_row), disk_details, FALSE, FALSE, 0);
    app->device_badge = make_label("●  Scanning…", NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->device_badge), "ldtm-status-badge");
    gtk_widget_set_valign(app->device_badge, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(target_row), app->device_badge, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target), target_row, FALSE, FALSE, 0);
    app->device_summary = make_label("Choose a physical test disk.", "ldtm-device-summary");
    gtk_label_set_ellipsize(GTK_LABEL(app->device_summary), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(target), app->device_summary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(shell), make_panel("ldtm-device-card", target), FALSE, FALSE, 0);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_name(body, "ldtm-body");
    GtkWidget *workflow = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_name(workflow, "ldtm-workflow");
    gtk_widget_set_size_request(workflow, 236, -1);
    gtk_container_set_border_width(GTK_CONTAINER(workflow), 10U);
    gtk_box_pack_start(GTK_BOX(workflow), make_label("Test workflow", "ldtm-section-title"), FALSE, FALSE, 0);
    app->build_button = make_operation_button("edit-delete-symbolic", "1. Build test disk", "Erase disk and create 21 layouts", "ldtm-operation-red");
    app->qualify_button = make_operation_button("applications-engineering-symbolic", "2. Qualify engines", "Defragment, Growth Defrag, verify", "ldtm-operation-cyan");
    app->verify_button = make_operation_button("emblem-ok-symbolic", "3. Verify files", "Read back retained test payloads", "ldtm-operation-purple");
    GtkWidget *buttons[] = { app->build_button, app->qualify_button, app->verify_button };
    const char *button_names[] = { "ldtm-build", "ldtm-qualify", "ldtm-verify" };
    for (size_t index = 0U; index < G_N_ELEMENTS(buttons); ++index) {
        gtk_widget_set_name(buttons[index], button_names[index]);
        gtk_widget_set_sensitive(buttons[index], FALSE);
        gtk_box_pack_start(GTK_BOX(workflow), buttons[index], FALSE, FALSE, 0);
    }
    g_signal_connect(app->build_button, "clicked", G_CALLBACK(build_clicked), app);
    g_signal_connect(app->qualify_button, "clicked", G_CALLBACK(qualify_clicked), app);
    g_signal_connect(app->verify_button, "clicked", G_CALLBACK(verify_clicked), app);
    GtkWidget *workflow_note = make_label("Build erases the selected disk.\nQualify and Verify use existing test media.", "ldtm-workflow-note");
    gtk_label_set_line_wrap(GTK_LABEL(workflow_note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(workflow_note), 28);
    gtk_box_pack_start(GTK_BOX(workflow), workflow_note, FALSE, FALSE, 0);
    GtkWidget *workflow_panel = make_panel("ldtm-workflow-card", workflow);
    gtk_widget_set_valign(workflow_panel, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(body), workflow_panel, FALSE, FALSE, 0);

    GtkWidget *results = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(results), 10U);
    GtkWidget *results_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(results_header), make_label("Filesystem results", "ldtm-section-title"), FALSE, FALSE, 0);
    app->result_summary = make_label("21 waiting", "ldtm-result-summary");
    gtk_label_set_ellipsize(GTK_LABEL(app->result_summary), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(app->result_summary), 1.0F);
    gtk_box_pack_end(GTK_BOX(results_header), app->result_summary, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(results), results_header, FALSE, FALSE, 0);
    app->result_stack = gtk_stack_new();
    gtk_widget_set_name(app->result_stack, "ldtm-result-stack");
    gtk_stack_set_homogeneous(GTK_STACK(app->result_stack), FALSE);
    gtk_stack_set_transition_type(GTK_STACK(app->result_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    GtkWidget *tabs = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(tabs), GTK_STACK(app->result_stack));
    gtk_style_context_add_class(gtk_widget_get_style_context(tabs), "ldtm-tabs");
    gtk_widget_set_halign(tabs, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(results), tabs, FALSE, FALSE, 0);

    GtkWidget *layouts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *flow = gtk_flow_box_new();
    gtk_widget_set_name(flow, "ldtm-filesystem-grid");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 2U);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 6U);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 8U);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 8U);
    gtk_widget_set_valign(flow, GTK_ALIGN_START);
    for (size_t index = 0U; index < ldtm_spec_count(); ++index)
        gtk_flow_box_insert(GTK_FLOW_BOX(flow), make_filesystem_tile(app, index), -1);
    GtkWidget *grid_scroll = make_scroll(flow, "ldtm-grid-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(grid_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(layouts), grid_scroll, TRUE, TRUE, 0);

    GtkWidget *inspector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(inspector), 8U);
    GtkWidget *inspector_heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->selected_title = make_label("FAT12", "ldtm-selected-title");
    gtk_box_pack_start(GTK_BOX(inspector_heading), app->selected_title, FALSE, FALSE, 0);
    app->selected_result = make_label("Waiting", NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->selected_result), "ldtm-fs-status");
    gtk_box_pack_start(GTK_BOX(inspector_heading), app->selected_result, FALSE, FALSE, 0);
    GtkWidget *file_details = gtk_button_new_with_label("Full details");
    gtk_style_context_add_class(gtk_widget_get_style_context(file_details), "ldtm-tool-button");
    gtk_widget_set_name(file_details, "ldtm-filesystem-details");
    g_signal_connect(file_details, "clicked", G_CALLBACK(filesystem_details_clicked), app);
    gtk_box_pack_end(GTK_BOX(inspector_heading), file_details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inspector), inspector_heading, FALSE, FALSE, 0);
    app->selected_meta = make_label("", "ldtm-selected-meta");
    gtk_label_set_ellipsize(GTK_LABEL(app->selected_meta), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(inspector), app->selected_meta, FALSE, FALSE, 0);
    app->selected_detail = make_label("Select a filesystem to inspect its result.", "ldtm-selected-detail");
    gtk_label_set_ellipsize(GTK_LABEL(app->selected_detail), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(inspector), app->selected_detail, FALSE, FALSE, 0);
    app->selected_panel = make_panel("ldtm-inspector", inspector);
    gtk_box_pack_start(GTK_BOX(layouts), app->selected_panel, FALSE, FALSE, 0);
    gtk_stack_add_titled(GTK_STACK(app->result_stack), layouts, "layouts", "Layouts");
    GtkWidget *tree = make_tree_view(app);
    gtk_stack_add_titled(GTK_STACK(app->result_stack), make_scroll(tree, "ldtm-details-scroll"), "details", "Details");
    GtkWidget *log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(log_view), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(log_view), 10);
    app->log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    g_object_set_data(G_OBJECT(app->log_buffer), "view", log_view);
    gtk_stack_add_titled(GTK_STACK(app->result_stack), make_scroll(log_view, "ldtm-log-scroll"), "log", "Live log");
    gtk_box_pack_start(GTK_BOX(results), app->result_stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(body), make_panel("ldtm-results-card", results), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(shell), body, TRUE, TRUE, 0);

    GtkWidget *activity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_name(activity, "ldtm-activity");
    gtk_container_set_border_width(GTK_CONTAINER(activity), 9U);
    GtkWidget *activity_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->activity_spinner = gtk_spinner_new();
    gtk_widget_set_no_show_all(app->activity_spinner, TRUE);
    gtk_box_pack_start(GTK_BOX(activity_header), app->activity_spinner, FALSE, FALSE, 0);
    app->operation_summary = make_label("Ready", "ldtm-operation-summary");
    gtk_label_set_ellipsize(GTK_LABEL(app->operation_summary), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(activity_header), app->operation_summary, TRUE, TRUE, 0);
    GtkWidget *view_log = gtk_button_new_with_label("View log");
    gtk_widget_set_name(view_log, "ldtm-view-log");
    g_signal_connect(view_log, "clicked", G_CALLBACK(view_log_clicked), app);
    gtk_box_pack_end(GTK_BOX(activity_header), view_log, FALSE, FALSE, 0);
    GtkWidget *copy_log = gtk_button_new_with_label("Copy log");
    gtk_style_context_add_class(gtk_widget_get_style_context(copy_log), "ldtm-tool-button");
    g_signal_connect(copy_log, "clicked", G_CALLBACK(copy_log_clicked), app);
    gtk_box_pack_end(GTK_BOX(activity_header), copy_log, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(activity), activity_header, FALSE, FALSE, 0);
    app->progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "Ready");
    gtk_box_pack_start(GTK_BOX(activity), app->progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(shell), make_panel("ldtm-operation-strip", activity), FALSE, FALSE, 0);
}

int ldtm_gui_main(int argc, char **argv)
{
    LdtmApp app = {0};
    gtk_init(&argc, &argv);
    create_test_media_window(&app);
    reset_filesystem_rows(&app);
    gtk_widget_show_all(app.window);
    refresh_devices(&app);
    gtk_main();
    cleanup_channels(&app);
    if (app.device_discovery_cancel != NULL) { g_cancellable_cancel(app.device_discovery_cancel); g_object_unref(app.device_discovery_cancel); }
    if (app.device_discovery != NULL) { g_subprocess_force_exit(app.device_discovery); g_object_unref(app.device_discovery); }
    g_object_unref(app.filesystem_store);
    g_object_unref(app.device_store);
    return 0;
}
