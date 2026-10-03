// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <infiltratr/design.h>

#include <gtk/gtk.h>

void ldtm_apply_mb_theme(void)
{
    GdkScreen *screen = gdk_screen_get_default();
    GtkCssProvider *provider;
    GString *css;
    GError *error = NULL;
    const InfiltratrTypography *typography = infiltratr_typography();
    const InfiltratrDesignMetrics *metrics = infiltratr_design_metrics();
    const InfiltratrThemePalette *palette =
        infiltratr_theme_resolve(INFILTRATR_THEME_NIGHT, true);

    if (screen == NULL || typography == NULL || metrics == NULL || palette == NULL)
        return;

    css = g_string_new(NULL);

    /* Keep the Test Media window visually inside the same Common-owned shell. */
    g_string_append_printf(
        css,
        "* { font-family: \"%s\"; color:#%06x; }"
        "window, dialog, .background { background-color:#%06x; }"
        "headerbar .title, .titlebar .title { font-family: \"%s\"; color:#%06x; }"
        "#ldtm-shell-header { min-height:44px; padding:6px 10px;"
        " background-color:#%06x; background-image:none;"
        " border-bottom:1px solid #%06x; box-shadow:none; }"
        "#ldtm-header-brand-title { color:#%06x; font-size:18px; font-weight:bold; }"
        "#ldtm-header-brand-subtitle { color:#%06x; font-size:10px; }"
        ".ldtm-window-control { min-width:30px; min-height:30px; padding:4px;"
        " color:#%06x; background-image:none; background-color:transparent;"
        " border:1px solid transparent; border-radius:%upx; box-shadow:none; }"
        ".ldtm-window-control:hover { background-color:#%06x; border-color:#%06x; }"
        ".ldtm-window-control-close:hover { background-color:#c84343; border-color:#e75b5b; color:#ffffff; }",
        typography->ui_family,
        (unsigned)palette->text_rgb,
        (unsigned)palette->background_rgb,
        typography->brand_family,
        (unsigned)palette->heading_rgb,
        (unsigned)palette->titlebar_rgb,
        (unsigned)palette->connection_border_rgb,
        (unsigned)palette->heading_rgb,
        (unsigned)palette->summary_rgb,
        (unsigned)palette->text_rgb,
        metrics->small_radius,
        (unsigned)palette->surface_hover_rgb,
        (unsigned)palette->accent_hover_rgb);

    /* The hero is an orientation strip, not a second title bar. */
    g_string_append_printf(
        css,
        "#ldtm-hero > border { background-color:#%06x; background-image:none;"
        " border:1px solid #%06x; border-radius:%upx; box-shadow:none; }"
        "#ldtm-hero-title { color:#%06x; font-size:20px; font-weight:bold; }"
        "#ldtm-hero-subtitle { color:#%06x; font-size:10px; }"
        "#ldtm-hero-badge { color:#%06x; background-color:#%06x;"
        " border:1px solid #%06x; border-radius:%upx; padding:4px 8px;"
        " font-size:9px; font-weight:bold; }"
        ".ldtm-icon-well { min-width:30px; min-height:30px; padding:5px;"
        " background-color:#%06x; background-image:none;"
        " border:1px solid #%06x; border-radius:%upx; box-shadow:none; }"
        ".ldtm-hero-icon { min-width:40px; min-height:40px; padding:6px;"
        " border-color:#%06x; background-color:#%06x; background-image:none; box-shadow:none; }",
        (unsigned)palette->card_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->heading_rgb,
        (unsigned)palette->summary_rgb,
        (unsigned)palette->heading_rgb,
        (unsigned)palette->surface_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->surface_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->connection_border_rgb,
        (unsigned)palette->surface_rgb);

    /* Limit colour to state and destructive meaning; decorative family colours were too noisy. */
    g_string_append_printf(
        css,
        ".ldtm-accent-cyan image,.ldtm-accent-green image,.ldtm-accent-purple image,"
        ".ldtm-accent-amber image,.ldtm-operation-cyan image,.ldtm-operation-purple image { color:#%06x; }"
        ".ldtm-operation-red image,.ldtm-danger-icon-well image { color:#ff6b6b; }"
        ".ldtm-card > border { background-color:#%06x; background-image:none;"
        " border:1px solid #%06x; border-radius:%upx; box-shadow:none; }"
        ".ldtm-device-card > border { border-color:#%06x; }"
        "#ldtm-section-title { color:#%06x; font-size:14px; font-weight:bold; }"
        "#ldtm-section-subtitle { color:#%06x; font-size:9px; }"
        "#ldtm-device-summary { color:#%06x; font-size:9px; }"
        ".ldtm-status-badge { padding:4px 8px; border-radius:%upx; font-weight:bold; font-size:9px; }"
        ".ldtm-badge-safe { color:#77e995; background-color:alpha(#43c965,0.10); border:1px solid alpha(#54db79,0.42); }"
        ".ldtm-badge-protected { color:#ff8383; background-color:alpha(#ef5350,0.10); border:1px solid alpha(#ef5350,0.42); }"
        "combobox button,entry,spinbutton { min-height:30px; background-color:#%06x;"
        " background-image:none; color:#%06x; border:1px solid #%06x;"
        " border-radius:%upx; box-shadow:none; }"
        "combobox button:hover,entry:focus,spinbutton:focus { border-color:#%06x; background-color:#%06x; }",
        (unsigned)palette->summary_rgb,
        (unsigned)palette->card_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->border_rgb,
        (unsigned)palette->heading_rgb,
        (unsigned)palette->detail_label_rgb,
        (unsigned)palette->note_rgb,
        metrics->small_radius,
        (unsigned)palette->input_rgb,
        (unsigned)palette->text_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->accent_hover_rgb,
        (unsigned)palette->surface_hover_rgb);

    /* Four facts remain useful, but they read as one quiet row rather than four feature cards. */
    g_string_append_printf(
        css,
        ".ldtm-stat-card > border { background-color:#%06x; background-image:none;"
        " border:1px solid #%06x; border-radius:%upx; box-shadow:none; }"
        ".ldtm-stat-card.ldtm-accent-green > border,"
        ".ldtm-stat-card.ldtm-accent-cyan > border,"
        ".ldtm-stat-card.ldtm-accent-purple > border,"
        ".ldtm-stat-card.ldtm-accent-amber > border { border-color:#%06x; }"
        ".ldtm-stat-card .ldtm-icon-well { min-width:24px; min-height:24px; padding:3px;"
        " background-color:transparent; border-color:transparent; box-shadow:none; }"
        ".ldtm-stat-card .ldtm-icon-well image { color:#%06x; }"
        ".ldtm-stat-title { color:#%06x; font-size:8px; }"
        ".ldtm-stat-value { color:#%06x; font-size:14px; font-weight:bold; }",
        (unsigned)palette->surface_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->border_rgb,
        (unsigned)palette->summary_rgb,
        (unsigned)palette->detail_label_rgb,
        (unsigned)palette->heading_rgb);

    /* The matrix is the main information surface: quiet tiles, status carries the colour. */
    g_string_append_printf(
        css,
        ".ldtm-fs-tile > border { background-color:#%06x; background-image:none;"
        " border:1px solid #%06x; border-left-width:2px; border-left-color:#%06x;"
        " border-radius:%upx; box-shadow:none; }"
        ".ldtm-fs-tile:hover > border { background-color:#%06x; border-color:#%06x; }"
        ".ldtm-fs-family-cyan > border,.ldtm-fs-family-green > border,"
        ".ldtm-fs-family-purple > border,.ldtm-fs-family-magenta > border,"
        ".ldtm-fs-family-amber > border,.ldtm-fs-family-neutral > border { border-left-color:#%06x; }"
        ".ldtm-fs-size { color:#%06x; font-size:8px; }"
        ".ldtm-fs-status { padding:1px 0; border-radius:0; font-size:8px; background-color:transparent; }"
        ".ldtm-fs-waiting .ldtm-fs-status { color:#%06x; background-color:transparent; }"
        ".ldtm-fs-success .ldtm-fs-status { color:#68e487; background-color:transparent; }"
        ".ldtm-fs-warning .ldtm-fs-status { color:#ffc45e; background-color:transparent; }"
        ".ldtm-fs-failure .ldtm-fs-status { color:#ff7474; background-color:transparent; }"
        "flowboxchild { padding:0; background:transparent; }"
        "flowboxchild:selected { background:transparent; }",
        (unsigned)palette->surface_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->surface_hover_rgb,
        (unsigned)palette->accent_hover_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->detail_label_rgb,
        (unsigned)palette->summary_rgb);

    /* Primary operations share one visual language. Red is reserved for destruction. */
    g_string_append_printf(
        css,
        ".ldtm-operation-button { min-height:64px; padding:0; border-radius:%upx;"
        " color:#%06x; box-shadow:none; background-image:none; }"
        ".ldtm-operation-cyan,.ldtm-operation-purple,button.ldtm-primary-action {"
        " background-color:#%06x; border:1px solid #%06x; }"
        ".ldtm-operation-cyan:hover,.ldtm-operation-purple:hover,button.ldtm-primary-action:hover {"
        " background-color:#%06x; border-color:#%06x; }"
        ".ldtm-operation-red,button.ldtm-destructive-action {"
        " background-color:#351719; border:1px solid #a84549; }"
        ".ldtm-operation-red:hover,button.ldtm-destructive-action:hover {"
        " background-color:#4b1d20; border-color:#e06165; }"
        ".ldtm-operation-title { color:#%06x; font-size:11px; font-weight:bold; }"
        ".ldtm-operation-subtitle { color:#%06x; font-size:8px; }"
        ".ldtm-operation-button:disabled,button.ldtm-primary-action:disabled,button.ldtm-destructive-action:disabled {"
        " opacity:1; background-color:#%06x; background-image:none; border-color:#%06x; }"
        ".ldtm-operation-button:disabled .ldtm-operation-title { color:#%06x; }"
        ".ldtm-operation-button:disabled .ldtm-operation-subtitle { color:#%06x; }"
        ".ldtm-operation-button:disabled image { color:#%06x; }"
        ".ldtm-operation-strip > border { background-color:#%06x; background-image:none;"
        " border:1px solid #%06x; border-radius:%upx; }"
        "#ldtm-operation-strip-title { color:#%06x; font-size:9px; font-weight:bold; }"
        "#ldtm-operation-summary { color:#%06x; font-size:9px; }"
        "progressbar trough { min-height:10px; background-color:#%06x; border:1px solid #%06x; border-radius:%upx; }"
        "progressbar progress { background-color:#%06x; background-image:none; border-radius:%upx; }"
        "progressbar text { color:#%06x; font-size:8pt; text-shadow:none; }",
        metrics->small_radius,
        (unsigned)palette->text_rgb,
        (unsigned)palette->surface_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->surface_hover_rgb,
        (unsigned)palette->accent_hover_rgb,
        (unsigned)palette->heading_rgb,
        (unsigned)palette->summary_rgb,
        (unsigned)palette->surface_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->detail_label_rgb,
        (unsigned)palette->note_rgb,
        (unsigned)palette->note_rgb,
        (unsigned)palette->surface_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->detail_label_rgb,
        (unsigned)palette->heading_rgb,
        (unsigned)palette->background_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->connection_rgb,
        metrics->small_radius,
        (unsigned)palette->text_rgb);

    /* Dense technical detail stays behind disclosure controls and uses plain surfaces. */
    g_string_append_printf(
        css,
        ".ldtm-detail-expander,.ldtm-log-expander { color:#%06x; font-weight:bold; }"
        ".ldtm-detail-expander > title,.ldtm-log-expander > title { padding:5px 3px; }"
        "treeview,textview,textview text,viewport,scrolledwindow { background-color:#%06x; color:#%06x; border-color:#%06x; }"
        "treeview.view header button { background-color:#%06x; background-image:none; border-color:#%06x; font-weight:bold; }"
        "treeview.view:selected { background-color:#%06x; color:#%06x; }"
        "scrollbar slider { background-color:#%06x; border-radius:%upx; min-width:7px; min-height:7px; }"
        "scrollbar slider:hover { background-color:#%06x; }"
        "tooltip { background-color:#%06x; color:#%06x; border:1px solid #%06x; }",
        (unsigned)palette->kicker_rgb,
        (unsigned)palette->surface_rgb,
        (unsigned)palette->text_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->card_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->selection_background_rgb,
        (unsigned)palette->selection_foreground_rgb,
        (unsigned)palette->note_rgb,
        metrics->small_radius,
        (unsigned)palette->status_border_rgb,
        (unsigned)palette->card_rgb,
        (unsigned)palette->text_rgb,
        (unsigned)palette->status_border_rgb);

    /* Dialogs retain a clear neutral escape and a red destructive confirmation. */
    g_string_append_printf(
        css,
        "dialog button { background-color:#%06x; background-image:none; color:#%06x;"
        " border:1px solid #%06x; border-radius:%upx; box-shadow:none; text-shadow:none; opacity:1; }"
        "dialog button:hover { background-color:#%06x; border-color:#%06x; }"
        "dialog button:disabled { background-color:#%06x; color:#%06x; border-color:#%06x; opacity:1; }"
        "dialog button.ldtm-destructive-action { background-color:#4b171a; color:#ffffff; border-color:#ef6265; font-weight:bold; }"
        "dialog button.ldtm-destructive-action:hover { background-color:#651f23; border-color:#ff8184; }"
        ".ldtm-danger-panel > border { background-color:#241214; background-image:none; border:1px solid #a63b3f; border-radius:%upx; }"
        ".ldtm-danger-icon-well { min-width:40px; min-height:40px; background-color:#321518; background-image:none; border-color:#a63b3f; }"
        "separator { background-color:#%06x; }",
        (unsigned)palette->surface_rgb,
        (unsigned)palette->text_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->surface_hover_rgb,
        (unsigned)palette->accent_hover_rgb,
        (unsigned)palette->background_rgb,
        (unsigned)palette->note_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->border_rgb);

    provider = gtk_css_provider_new();
    if (gtk_css_provider_load_from_data(provider, css->str, -1, &error)) {
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 50);
    }
    if (error != NULL) g_error_free(error);
    g_object_unref(provider);
    g_string_free(css, TRUE);
}
