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
    GtkWidget *readiness_summary;
    GtkWidget *operation_summary;
    GtkListStore *filesystem_store;
    GtkWidget *build_button;
    GtkWidget *verify_button;
    GtkWidget *progress;
    GtkTextBuffer *log_buffer;
    GPid child_pid;
    GIOChannel *stdout_channel;
    GIOChannel *stderr_channel;
    guint stdout_watch;
    guint stderr_watch;
    guint child_watch;
    guint completed_rows;
    gboolean worker_running;
} LdtmApp;

static char *pair_value(const char *line, const char *key) {
    char *needle;
    const char *found;
    const char *cursor;
    GString *value;
    needle = g_strdup_printf("%s=\"", key);
    found = strstr(line, needle);
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
            g_string_append_c(value, *cursor);
            ++cursor;
        }
    }
    return g_string_free(value, FALSE);
}

static void show_message(GtkWindow *parent, GtkMessageType type,
                         const char *primary, const char *secondary) {
    GtkWidget *dialog = gtk_message_dialog_new(parent,
                                                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                type, GTK_BUTTONS_CLOSE, "%s", primary);
    if (secondary != NULL && *secondary != '\0') {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary);
    }
    (void)gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void append_log(LdtmApp *app, const char *text) {
    GtkTextIter end;
    GtkTextMark *mark;
    GtkTextView *view = GTK_TEXT_VIEW(g_object_get_data(G_OBJECT(app->log_buffer), "view"));
    gtk_text_buffer_get_end_iter(app->log_buffer, &end);
    gtk_text_buffer_insert(app->log_buffer, &end, text, -1);
    gtk_text_buffer_get_end_iter(app->log_buffer, &end);
    mark = gtk_text_buffer_create_mark(app->log_buffer, NULL, &end, FALSE);
    if (view != NULL) gtk_text_view_scroll_mark_onscreen(view, mark);
    gtk_text_buffer_delete_mark(app->log_buffer, mark);
}

static const char *display_result(const char *status) {
    if (status == NULL) return "Unknown";
    if (strcmp(status, "populated") == 0) return "✓ Populated";
    if (strcmp(status, "formatted") == 0) return "✓ Formatted";
    if (strcmp(status, "formatted-unpopulated") == 0) return "⚠ Formatted only";
    if (strcmp(status, "skipped") == 0) return "— Skipped";
    if (strcmp(status, "format-failed") == 0) return "✕ Format failed";
    if (strcmp(status, "verify-ok") == 0) return "✓ Verified";
    if (strcmp(status, "verify-fail") == 0) return "✕ Verification failed";
    if (strcmp(status, "verify-skip") == 0) return "— Verification skipped";
    return status;
}

static void update_progress_text(LdtmApp *app, const char *filesystem, const char *result) {
    char text[192];
    const guint total = (guint)ldtm_spec_count();
    const double fraction = total > 0U ? (double)app->completed_rows / (double)total : 0.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), fraction);
    if (filesystem != NULL && result != NULL) {
        (void)snprintf(text, sizeof(text), "%u / %u — %s: %s",
                       app->completed_rows, total, filesystem, result);
    } else {
        (void)snprintf(text, sizeof(text), "%u / %u", app->completed_rows, total);
    }
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), text);
}

static void update_filesystem_status(LdtmApp *app, const char *key,
                                     const char *status, const char *detail) {
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->filesystem_store), &iter);
    while (valid) {
        char *row_key = NULL;
        char *old_result = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->filesystem_store), &iter,
                           LDTM_FS_COL_KEY, &row_key,
                           LDTM_FS_COL_RESULT, &old_result,
                           -1);
        if (g_strcmp0(row_key, key) == 0) {
            const char *shown = display_result(status);
            if (g_strcmp0(old_result, "Waiting") == 0 && app->completed_rows < ldtm_spec_count()) {
                ++app->completed_rows;
            }
            gtk_list_store_set(app->filesystem_store, &iter,
                               LDTM_FS_COL_RESULT, shown,
                               LDTM_FS_COL_DETAIL, detail != NULL ? detail : "",
                               -1);
            update_progress_text(app, key, shown);
            g_free(old_result);
            g_free(row_key);
            return;
        }
        g_free(old_result);
        g_free(row_key);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->filesystem_store), &iter);
    }
}

static void parse_worker_status(LdtmApp *app, const char *line) {
    if (g_str_has_prefix(line, "LDTM_STATUS\t")) {
        gchar **fields = g_strsplit(line, "\t", 4);
        if (g_strv_length(fields) >= 4U) {
            g_strchomp(fields[3]);
            update_filesystem_status(app, fields[1], fields[2], fields[3]);
        }
        g_strfreev(fields);
    } else if (g_str_has_prefix(line, "=== ")) {
        const char *start = line + 4;
        const char *colon = strchr(start, ':');
        if (colon != NULL && colon > start) {
            char *filesystem = g_strndup(start, (gsize)(colon - start));
            char *message = g_strdup_printf("Working on %s…", filesystem);
            gtk_label_set_text(GTK_LABEL(app->operation_summary), message);
            g_free(message);
            g_free(filesystem);
        }
    }
}

static gboolean channel_watch(GIOChannel *channel, GIOCondition condition, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    if ((condition & (G_IO_IN | G_IO_HUP)) != 0) {
        for (;;) {
            gchar *line = NULL;
            gsize length = 0U;
            GIOStatus status = g_io_channel_read_line(channel, &line, &length, NULL, NULL);
            if (status == G_IO_STATUS_NORMAL && line != NULL) {
                (void)length;
                append_log(app, line);
                parse_worker_status(app, line);
                g_free(line);
                continue;
            }
            g_free(line);
            if (status == G_IO_STATUS_AGAIN) return TRUE;
            break;
        }
    }
    return (condition & (G_IO_ERR | G_IO_NVAL | G_IO_HUP)) == 0;
}

static void set_worker_controls(LdtmApp *app, gboolean running) {
    GtkTreeIter iter;
    gboolean safe = FALSE;
    app->worker_running = running;
    if (gtk_combo_box_get_active_iter(app->device_combo, &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->device_store), &iter,
                           LDTM_DEVICE_COL_SAFE, &safe, -1);
    }
    gtk_widget_set_sensitive(app->build_button, !running && safe);
    gtk_widget_set_sensitive(app->verify_button, !running && safe);
    gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), !running);
    if (running) {
        app->completed_rows = 0U;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "0 / 21 — starting…");
        gtk_label_set_text(GTK_LABEL(app->operation_summary), "Privileged worker starting…");
    } else {
        gtk_label_set_text(GTK_LABEL(app->operation_summary), "Idle");
    }
}

static void cleanup_channels(LdtmApp *app) {
    if (app->stdout_watch != 0U) {
        g_source_remove(app->stdout_watch);
        app->stdout_watch = 0U;
    }
    if (app->stderr_watch != 0U) {
        g_source_remove(app->stderr_watch);
        app->stderr_watch = 0U;
    }
    if (app->stdout_channel != NULL) {
        g_io_channel_unref(app->stdout_channel);
        app->stdout_channel = NULL;
    }
    if (app->stderr_channel != NULL) {
        g_io_channel_unref(app->stderr_channel);
        app->stderr_channel = NULL;
    }
}

static void worker_finished(GPid pid, gint status, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    char message[128];
    cleanup_channels(app);
    app->child_watch = 0U;
    app->child_pid = 0;
    g_spawn_close_pid(pid);
    set_worker_controls(app, FALSE);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 1.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "Completed");
        gtk_label_set_text(GTK_LABEL(app->operation_summary), "Operation completed successfully");
        (void)snprintf(message, sizeof(message), "\nWorker completed successfully.\n");
        append_log(app, message);
    } else {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "Operation failed");
        gtk_label_set_text(GTK_LABEL(app->operation_summary), "Operation failed — see live log");
        (void)snprintf(message, sizeof(message), "\nWorker failed (status %d).\n", status);
        append_log(app, message);
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                     "Test-media operation failed",
                     "See the live log for the command that failed.");
    }
}

static char *selected_device(LdtmApp *app) {
    GtkTreeIter iter;
    char *path = NULL;
    if (!gtk_combo_box_get_active_iter(app->device_combo, &iter)) return NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->device_store), &iter,
                       LDTM_DEVICE_COL_PATH, &path, -1);
    return path;
}

static gboolean spawn_worker(LdtmApp *app, const char *operation,
                             const char *device, gboolean confirmed,
                             const char *fingerprint) {
    gchar *self = g_file_read_link("/proc/self/exe", NULL);
    gchar *argv[10];
    gint stdout_fd = -1;
    gint stderr_fd = -1;
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
            for (guint index = 0U; index < arg; ++index) g_free(argv[index]);
            return FALSE;
        }
        argv[arg++] = g_strdup("--confirmed");
        argv[arg++] = g_strdup(device);
        argv[arg++] = g_strdup("--fingerprint");
        argv[arg++] = g_strdup(fingerprint);
    }
    argv[arg] = NULL;
    started = g_spawn_async_with_pipes(NULL, argv, NULL,
                                       G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
                                       NULL, NULL, &app->child_pid,
                                       NULL, &stdout_fd, &stderr_fd, &error);
    for (guint index = 0U; index < arg; ++index) g_free(argv[index]);
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
                                      G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                      channel_watch, app);
    app->stderr_watch = g_io_add_watch(app->stderr_channel,
                                      G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                      channel_watch, app);
    app->child_watch = g_child_watch_add(app->child_pid, worker_finished, app);
    set_worker_controls(app, TRUE);
    return TRUE;
}

static gboolean confirmation_dialog(LdtmApp *app, const char *device) {
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *label;
    GtkWidget *entry;
    GtkWidget *accept_button;
    char *expected;
    gboolean accepted = FALSE;
    gint response;
    expected = g_strdup_printf("DESTROY %s", device);
    dialog = gtk_dialog_new_with_buttons("Destroy and build test disk",
                                         GTK_WINDOW(app->window),
                                         GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                         "_Cancel", GTK_RESPONSE_CANCEL,
                                         "_Destroy and Build", GTK_RESPONSE_ACCEPT,
                                         NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 620, -1);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    label = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    {
        char *markup = g_markup_printf_escaped(
            "<big><b>Erase the entire disk %s?</b></big>\n\n"
            "Every existing partition and file on this device will be destroyed. "
            "The privileged worker performs the complete safety check again before writing anything.\n\n"
            "Type this exact confirmation to continue:\n\n<b>%s</b>",
            device, expected);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
    }
    entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), expected);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 14U);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 14U);
    accept_button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    if (accept_button != NULL) {
        GtkStyleContext *context = gtk_widget_get_style_context(accept_button);
        gtk_style_context_add_class(context, "destructive-action");
    }
    gtk_widget_show_all(dialog);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT &&
        g_strcmp0(gtk_entry_get_text(GTK_ENTRY(entry)), expected) == 0) {
        accepted = TRUE;
    } else if (response == GTK_RESPONSE_ACCEPT) {
        show_message(GTK_WINDOW(dialog), GTK_MESSAGE_ERROR,
                     "Confirmation did not match",
                     "No disk changes were made.");
    }
    gtk_widget_destroy(dialog);
    g_free(expected);
    return accepted;
}

static void reset_filesystem_rows(LdtmApp *app) {
    size_t index;
    guint ready = 0U;
    guint package_missing = 0U;
    guint unavailable = 0U;
    guint manual = 0U;
    char summary[256];
    gtk_list_store_clear(app->filesystem_store);
    for (index = 0U; index < ldtm_spec_count(); ++index) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
        GtkTreeIter iter;
        char size[32];
        char payload[32];
        char creator_detail[256];
        char detail[512];
        const gboolean available = ldtm_spec_creator_available(spec, creator_detail,
                                                                sizeof(creator_detail)) != 0;
        const char *availability;
        (void)snprintf(size, sizeof(size), "%u MiB", spec->size_mib);
        if (spec->payload_mib == 0U) (void)snprintf(payload, sizeof(payload), "—");
        else (void)snprintf(payload, sizeof(payload), "%u MiB", spec->payload_mib);
        if (spec->creator == LDTM_CREATOR_MANUAL) {
            availability = "○ Reserved / manual";
            ++manual;
            (void)snprintf(detail, sizeof(detail), "%s", spec->note);
        } else if (available) {
            availability = "✓ Ready";
            ++ready;
            (void)snprintf(detail, sizeof(detail), "%s", creator_detail);
        } else if (spec->package_hint != NULL && *spec->package_hint != '\0') {
            availability = "⚠ Optional package missing";
            ++package_missing;
            (void)snprintf(detail, sizeof(detail),
                           "%s. Install package '%s' to enable this test slot.",
                           creator_detail, spec->package_hint);
        } else {
            availability = "— No standard creator";
            ++unavailable;
            (void)snprintf(detail, sizeof(detail),
                           "No supported creator is installed for this format. The partition slot is reserved and the rest of the build continues.%s%s",
                           (spec->note != NULL && *spec->note != '\0') ? " " : "",
                           (spec->note != NULL) ? spec->note : "");
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
                           LDTM_FS_COL_DETAIL, detail,
                           -1);
    }
    (void)snprintf(summary, sizeof(summary),
                   "%u ready  •  %u package-missing  •  %u unavailable  •  %u manual/reserved",
                   ready, package_missing, unavailable, manual);
    gtk_label_set_text(GTK_LABEL(app->readiness_summary), summary);
    app->completed_rows = 0U;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "Idle");
}

static void device_changed(GtkComboBox *combo, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    GtkTreeIter iter;
    char *summary = NULL;
    gboolean safe = FALSE;
    (void)combo;
    if (gtk_combo_box_get_active_iter(app->device_combo, &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->device_store), &iter,
                           LDTM_DEVICE_COL_SUMMARY, &summary,
                           LDTM_DEVICE_COL_SAFE, &safe, -1);
    }
    gtk_label_set_text(GTK_LABEL(app->device_summary), summary != NULL ? summary : "No disk selected.");
    g_free(summary);
    if (!app->worker_running) {
        gtk_widget_set_sensitive(app->build_button, safe);
        gtk_widget_set_sensitive(app->verify_button, safe);
    }
    reset_filesystem_rows(app);
}

static void refresh_devices(LdtmApp *app) {
    gchar *stdout_text = NULL;
    gchar *stderr_text = NULL;
    gint exit_status = 0;
    GError *error = NULL;
    gchar *argv[] = {
        (gchar *)"lsblk", (gchar *)"-d", (gchar *)"-b", (gchar *)"-n", (gchar *)"-P",
        (gchar *)"-o", (gchar *)"PATH,SIZE,MODEL,SERIAL,WWN,TRAN,RM,RO", NULL
    };
    gchar **lines;
    gint first_safe = -1;
    gint row = 0;
    gtk_list_store_clear(app->device_store);
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &stdout_text, &stderr_text, &exit_status, &error) ||
        !g_spawn_check_wait_status(exit_status, &error)) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                     "Could not enumerate physical disks",
                     error != NULL ? error->message : stderr_text);
        g_clear_error(&error);
        g_free(stdout_text);
        g_free(stderr_text);
        return;
    }
    lines = g_strsplit(stdout_text, "\n", -1);
    for (guint index = 0U; lines[index] != NULL; ++index) {
        char *path;
        char *size_text;
        char *model;
        char *serial;
        char *wwn;
        char *transport;
        char *rm_text;
        char *ro_text;
        uint64_t bytes = 0U;
        uint64_t removable_value = 0U;
        uint64_t readonly_value = 0U;
        int removable;
        int readonly;
        gboolean system_disk;
        gboolean field_media;
        gboolean enough;
        gboolean stable_identity;
        gboolean safe;
        char *display;
        char *summary;
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
            !infiltratr_parse_u64_range(ro_text, 10U, 0U, 1U, &readonly_value))
            continue;
        removable = (int)removable_value;
        readonly = (int)readonly_value;
        system_disk = ldtm_is_system_disk(path) != 0;
        field_media = ldtm_transport_is_field_media(removable, transport) != 0;
        enough = bytes >= ldtm_required_capacity_bytes();
        stable_identity = *serial != '\0' || *wwn != '\0';
        safe = !system_disk && readonly == 0 && field_media && enough &&
               stable_identity;
        display = g_strdup_printf("%s   %.1f GiB   %s%s",
                                  path, (double)bytes / (double)LDTM_GIB,
                                  (*model != '\0') ? model : "unknown model",
                                  system_disk ? "   PROTECTED SYSTEM DISK" :
                                  (safe ? "   field-media candidate" : ""));
        summary = g_strdup_printf(
            "Device: %s\nModel: %s    Serial: %s    WWN: %s\nSize: %.1f GiB    Transport: %s    RM=%d    RO=%d\n%s",
            path, *model != '\0' ? model : "unknown",
            *serial != '\0' ? serial : "unknown",
            *wwn != '\0' ? wwn : "unknown",
            (double)bytes / (double)LDTM_GIB,
            *transport != '\0' ? transport : "unknown",
            removable, readonly,
            system_disk ? "PROTECTED — this disk backs active system storage." :
            (!field_media ? "Not accepted as removable/USB/MMC field media." :
             (!enough ? "Too small for the full 21-partition test layout." :
              (!stable_identity ? "PROTECTED — no stable serial or WWN is available to bind destructive confirmation." :
               "✓ Safety pre-check passed. The privileged worker verifies the device identity again before writing."))));
        gtk_list_store_append(app->device_store, &iter);
        gtk_list_store_set(app->device_store, &iter,
                           LDTM_DEVICE_COL_PATH, path,
                           LDTM_DEVICE_COL_DISPLAY, display,
                           LDTM_DEVICE_COL_SAFE, safe,
                           LDTM_DEVICE_COL_SUMMARY, summary,
                           -1);
        if (safe && first_safe < 0) first_safe = row;
        ++row;
        g_free(summary);
        g_free(display);
        g_free(ro_text);
        g_free(rm_text);
        g_free(transport);
        g_free(wwn);
        g_free(serial);
        g_free(model);
        g_free(size_text);
        g_free(path);
    }
    g_strfreev(lines);
    g_free(stdout_text);
    g_free(stderr_text);
    if (first_safe >= 0) gtk_combo_box_set_active(app->device_combo, first_safe);
    else if (row > 0) gtk_combo_box_set_active(app->device_combo, 0);
}

static void refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    refresh_devices((LdtmApp *)user_data);
}

static void prepare_results_for_operation(LdtmApp *app) {
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->filesystem_store), &iter);
    while (valid) {
        gtk_list_store_set(app->filesystem_store, &iter, LDTM_FS_COL_RESULT, "Waiting", -1);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->filesystem_store), &iter);
    }
    app->completed_rows = 0U;
}

static void build_clicked(GtkButton *button, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    char *device = selected_device(app);
    char fingerprint[65];
    (void)button;
    if (device == NULL) return;
    if (ldtm_device_fingerprint(device, fingerprint) != 0) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                     "Could not bind the selected physical disk",
                     "The disk identity could not be read. No disk changes were made.");
        g_free(device);
        return;
    }
    if (confirmation_dialog(app, device)) {
        gtk_text_buffer_set_text(app->log_buffer, "", -1);
        prepare_results_for_operation(app);
        append_log(app, "Preparing destructive filesystem test media...\n");
        (void)spawn_worker(app, "prepare", device, TRUE, fingerprint);
    }
    g_free(device);
}

static void verify_clicked(GtkButton *button, gpointer user_data) {
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
                                             int model_column, gint min_width,
                                             gboolean expand) {
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
        title, renderer, "text", model_column, NULL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_min_width(column, min_width);
    gtk_tree_view_column_set_expand(column, expand);
    gtk_tree_view_append_column(view, column);
    return column;
}

static GtkWidget *make_tree_view(LdtmApp *app) {
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
    gtk_tree_view_set_grid_lines(tree, GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    gtk_tree_view_set_enable_search(tree, TRUE);
    gtk_tree_view_set_search_column(tree, LDTM_FS_COL_KEY);
    gtk_tree_view_set_tooltip_column(tree, LDTM_FS_COL_DETAIL);
    return view;
}

static GtkWidget *make_device_combo(LdtmApp *app) {
    GtkWidget *combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(app->device_store));
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), renderer,
                                   "text", LDTM_DEVICE_COL_DISPLAY, NULL);
    return combo;
}

static void window_destroyed(GtkWidget *widget, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    (void)widget;
    if (app->child_pid != 0) (void)kill(app->child_pid, SIGTERM);
    gtk_main_quit();
}

static GtkWidget *make_section_label(const char *text) {
    GtkWidget *label = gtk_label_new(NULL);
    char *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    g_free(markup);
    return label;
}

int ldtm_gui_main(int argc, char **argv) {
    LdtmApp app;
    GtkWidget *header;
    GtkWidget *refresh_button;
    GtkWidget *outer;
    GtkWidget *intro;
    GtkWidget *device_frame;
    GtkWidget *device_box;
    GtkWidget *device_row;
    GtkWidget *readiness_row;
    GtkWidget *content_paned;
    GtkWidget *table_box;
    GtkWidget *fs_scroll;
    GtkWidget *tree;
    GtkWidget *lower_box;
    GtkWidget *action_row;
    GtkWidget *action_spacer;
    GtkWidget *log_scroll;
    GtkWidget *log_view;
    GtkStyleContext *style;
    GdkGeometry geometry;

    memset(&app, 0, sizeof(app));
    gtk_init(&argc, &argv);

    app.device_store = gtk_list_store_new(LDTM_DEVICE_N_COLUMNS,
                                          G_TYPE_STRING, G_TYPE_STRING,
                                          G_TYPE_BOOLEAN, G_TYPE_STRING);
    app.filesystem_store = gtk_list_store_new(LDTM_FS_N_COLUMNS,
                                              G_TYPE_STRING, G_TYPE_STRING,
                                              G_TYPE_STRING, G_TYPE_STRING,
                                              G_TYPE_STRING, G_TYPE_STRING,
                                              G_TYPE_STRING, G_TYPE_STRING);

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Defragmenter Test Media");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1240, 820);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    geometry.min_width = 820;
    geometry.min_height = 620;
    gtk_window_set_geometry_hints(GTK_WINDOW(app.window), app.window, &geometry,
                                  GDK_HINT_MIN_SIZE);
    g_signal_connect(app.window, "destroy", G_CALLBACK(window_destroyed), &app);

    header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Defragmenter Test Media");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header),
                                "Dedicated destructive filesystem test-media builder");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(refresh_button, "Refresh physical disk list");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), refresh_button);
    gtk_window_set_titlebar(GTK_WINDOW(app.window), header);
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(refresh_clicked), &app);

    outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12U);
    gtk_container_add(GTK_CONTAINER(app.window), outer);

    intro = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(intro),
        "Builds <b>sacrificial test filesystems</b> with deterministic fragmented payloads. "
        "Unavailable creators are reserved and skipped; they do not stop the rest of the test disk.");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_xalign(GTK_LABEL(intro), 0.0F);
    style = gtk_widget_get_style_context(intro);
    gtk_style_context_add_class(style, "dim-label");
    gtk_box_pack_start(GTK_BOX(outer), intro, FALSE, FALSE, 0U);

    device_frame = gtk_frame_new("Physical test disk");
    gtk_frame_set_shadow_type(GTK_FRAME(device_frame), GTK_SHADOW_IN);
    gtk_box_pack_start(GTK_BOX(outer), device_frame, FALSE, FALSE, 0U);
    device_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(device_box), 10U);
    gtk_container_add(GTK_CONTAINER(device_frame), device_box);

    device_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(device_box), device_row, FALSE, FALSE, 0U);
    app.device_combo = GTK_COMBO_BOX(make_device_combo(&app));
    gtk_widget_set_hexpand(GTK_WIDGET(app.device_combo), TRUE);
    gtk_box_pack_start(GTK_BOX(device_row), GTK_WIDGET(app.device_combo), TRUE, TRUE, 0U);
    g_signal_connect(app.device_combo, "changed", G_CALLBACK(device_changed), &app);

    app.device_summary = gtk_label_new("No disk selected.");
    gtk_label_set_xalign(GTK_LABEL(app.device_summary), 0.0F);
    gtk_label_set_selectable(GTK_LABEL(app.device_summary), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(app.device_summary), TRUE);
    gtk_box_pack_start(GTK_BOX(device_box), app.device_summary, FALSE, FALSE, 0U);

    readiness_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), readiness_row, FALSE, FALSE, 0U);
    gtk_box_pack_start(GTK_BOX(readiness_row), make_section_label("Filesystem readiness"), FALSE, FALSE, 0U);
    app.readiness_summary = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app.readiness_summary), 0.0F);
    style = gtk_widget_get_style_context(app.readiness_summary);
    gtk_style_context_add_class(style, "dim-label");
    gtk_box_pack_start(GTK_BOX(readiness_row), app.readiness_summary, TRUE, TRUE, 0U);

    content_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_position(GTK_PANED(content_paned), 430);
    gtk_box_pack_start(GTK_BOX(outer), content_paned, TRUE, TRUE, 0U);

    table_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    reset_filesystem_rows(&app);
    tree = make_tree_view(&app);
    fs_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fs_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(fs_scroll, TRUE);
    gtk_widget_set_vexpand(fs_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(fs_scroll), tree);
    gtk_box_pack_start(GTK_BOX(table_box), fs_scroll, TRUE, TRUE, 0U);
    gtk_paned_pack1(GTK_PANED(content_paned), table_box, TRUE, FALSE);

    lower_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(lower_box), 2U);
    action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(lower_box), action_row, FALSE, FALSE, 0U);

    app.operation_summary = gtk_label_new("Idle");
    gtk_label_set_xalign(GTK_LABEL(app.operation_summary), 0.0F);
    gtk_box_pack_start(GTK_BOX(action_row), app.operation_summary, FALSE, FALSE, 0U);
    action_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(action_spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(action_row), action_spacer, TRUE, TRUE, 0U);

    app.verify_button = gtk_button_new_with_label("Verify After Defrag");
    style = gtk_widget_get_style_context(app.verify_button);
    gtk_style_context_add_class(style, "suggested-action");
    gtk_widget_set_sensitive(app.verify_button, FALSE);
    gtk_box_pack_start(GTK_BOX(action_row), app.verify_button, FALSE, FALSE, 0U);
    g_signal_connect(app.verify_button, "clicked", G_CALLBACK(verify_clicked), &app);

    app.build_button = gtk_button_new_with_label("Build Test Disk");
    style = gtk_widget_get_style_context(app.build_button);
    gtk_style_context_add_class(style, "destructive-action");
    gtk_widget_set_sensitive(app.build_button, FALSE);
    gtk_box_pack_start(GTK_BOX(action_row), app.build_button, FALSE, FALSE, 0U);
    g_signal_connect(app.build_button, "clicked", G_CALLBACK(build_clicked), &app);

    app.progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app.progress), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.progress), "Idle");
    gtk_box_pack_start(GTK_BOX(lower_box), app.progress, FALSE, FALSE, 0U);

    gtk_box_pack_start(GTK_BOX(lower_box), make_section_label("Live operation log"), FALSE, FALSE, 0U);
    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(log_view), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(log_view), 6);
    app.log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    g_object_set_data(G_OBJECT(app.log_buffer), "view", log_view);
    log_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(log_scroll, TRUE);
    gtk_widget_set_vexpand(log_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(log_scroll), log_view);
    gtk_box_pack_start(GTK_BOX(lower_box), log_scroll, TRUE, TRUE, 0U);
    gtk_paned_pack2(GTK_PANED(content_paned), lower_box, TRUE, FALSE);

    refresh_devices(&app);
    gtk_widget_show_all(app.window);
    gtk_main();

    cleanup_channels(&app);
    g_object_unref(app.filesystem_store);
    g_object_unref(app.device_store);
    return 0;
}
