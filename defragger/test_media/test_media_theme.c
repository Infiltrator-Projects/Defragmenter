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

    /* Common owns identity, semantics and structural metrics. */
    g_string_append_printf(
        css,
        "* { font-family: \"%s\"; color: #%06x; }"
        "window, dialog, .background { background-color: #%06x; }"
        "headerbar .title, .titlebar .title { font-family: \"%s\"; color: #%06x; }"
        "#ldtm-shell-header { min-height:44px; padding:6px 10px;"
        " background-image:linear-gradient(to right,#%06x,#%06x 52%%,#%06x);"
        " border-bottom:1px solid #%06x; box-shadow:0 2px 12px alpha(#000000,0.45); }"
        "#ldtm-header-brand-title { color:#%06x; font-size:18px; font-weight:bold; }"
        "#ldtm-header-brand-subtitle { color:#%06x; font-size:10px; }"
        ".ldtm-window-control { min-width:30px; min-height:30px; padding:4px; color:#%06x;"
        " background-image:none; background-color:transparent; border:1px solid transparent;"
        " border-radius:8px; box-shadow:none; }"
        ".ldtm-window-control:hover { background-color:#%06x; border-color:#%06x; }"
        ".ldtm-window-control-close:hover { background-color:#c84343; border-color:#e75b5b; color:#ffffff; }",
        typography->ui_family,
        (unsigned)palette->text_rgb,
        (unsigned)palette->background_rgb,
        typography->brand_family,
        (unsigned)palette->heading_rgb,
        (unsigned)palette->titlebar_rgb,
        (unsigned)palette->connection_rgb,
        (unsigned)palette->card_rgb,
        (unsigned)palette->connection_border_rgb,
        (unsigned)palette->heading_rgb,
        (unsigned)palette->summary_rgb,
        (unsigned)palette->text_rgb,
        (unsigned)palette->surface_hover_rgb,
        (unsigned)palette->accent_hover_rgb);

    /* Graphical hero: the page should read visually before it is read as text. */
    g_string_append_printf(
        css,
        "#ldtm-hero > border {"
        " background-image:linear-gradient(110deg,#083f73 0%%,#143d86 38%%,#35206f 72%%,#4a164d 100%%);"
        " border:1px solid #%06x; border-radius:14px;"
        " box-shadow:0 5px 20px alpha(#000000,0.42),inset 0 1px alpha(#ffffff,0.08); }"
        "#ldtm-hero-title { color:#ffffff; font-size:26px; font-weight:bold; }"
        "#ldtm-hero-subtitle { color:#%06x; font-size:12px; }"
        "#ldtm-hero-badge { color:#dff7ff; background-color:alpha(#071525,0.62);"
        " border:1px solid alpha(#59d7ff,0.55); border-radius:16px; padding:7px 12px;"
        " font-size:10px; font-weight:bold; }"
        ".ldtm-icon-well { min-width:36px; min-height:36px; padding:7px;"
        " background-image:linear-gradient(to bottom right,#111d29,#0a1118);"
        " border:1px solid #%06x; border-radius:12px;"
        " box-shadow:inset 0 1px alpha(#ffffff,0.06),0 2px 8px alpha(#000000,0.35); }"
        ".ldtm-hero-icon { min-width:58px; min-height:58px; padding:10px; border-color:#36c9ff;"
        " background-image:linear-gradient(to bottom right,#0a4770,#11162c);"
        " box-shadow:0 0 22px alpha(#00bdf2,0.28),inset 0 1px alpha(#ffffff,0.12); }"
        ".ldtm-accent-cyan image,.ldtm-operation-cyan image { color:#31c8f4; }"
        ".ldtm-accent-green image { color:#54db79; }"
        ".ldtm-accent-purple image,.ldtm-operation-purple image { color:#b36cff; }"
        ".ldtm-accent-amber image { color:#ffb52f; }"
        ".ldtm-operation-red image,.ldtm-danger-icon-well image { color:#ff6b6b; }",
        (unsigned)palette->status_border_rgb,
        (unsigned)palette->summary_rgb,
        (unsigned)palette->connection_border_rgb);

    /* Cards and selected-device surface. */
    g_string_append_printf(
        css,
        ".ldtm-card > border { background-image:linear-gradient(to bottom right,#%06x,#%06x);"
        " border:1px solid #%06x; border-radius:12px;"
        " box-shadow:0 3px 12px alpha(#000000,0.26),inset 0 1px alpha(#ffffff,0.025); }"
        ".ldtm-device-card > border { border-color:#1b789f; }"
        "#ldtm-section-title { color:#%06x; font-size:14px; font-weight:bold; }"
        "#ldtm-section-subtitle { color:#%06x; font-size:10px; }"
        "#ldtm-device-summary { color:#%06x; font-size:10px; }"
        ".ldtm-status-badge { padding:5px 10px; border-radius:14px; font-weight:bold; font-size:10px; }"
        ".ldtm-badge-safe { color:#77e995; background-color:alpha(#43c965,0.12); border:1px solid alpha(#54db79,0.48); }"
        ".ldtm-badge-protected { color:#ff8383; background-color:alpha(#ef5350,0.12); border:1px solid alpha(#ef5350,0.46); }"
        "combobox button,entry,spinbutton { min-height:30px;"
        " background-image:linear-gradient(to bottom,#%06x,#%06x); color:#%06x;"
        " border:1px solid #%06x; border-radius:%upx; box-shadow:inset 0 1px alpha(#ffffff,0.035); }"
        "combobox button:hover,entry:focus,spinbutton:focus { border-color:#%06x; background-color:#%06x; }",
        (unsigned)palette->card_rgb,
        (unsigned)palette->surface_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->heading_rgb,
        (unsigned)palette->detail_label_rgb,
        (unsigned)palette->note_rgb,
        (unsigned)palette->input_rgb,
        (unsigned)palette->surface_rgb,
        (unsigned)palette->text_rgb,
        (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->accent_hover_rgb,
        (unsigned)palette->surface_hover_rgb);

    /* Visual summary cards. */
    g_string_append(
        css,
        ".ldtm-stat-card > border { background-image:linear-gradient(120deg,#101820,#0d1319);"
        " border:1px solid #263644; border-radius:11px; box-shadow:inset 0 1px alpha(#ffffff,0.035); }"
        ".ldtm-stat-card.ldtm-accent-green > border { border-color:alpha(#54db79,0.48); }"
        ".ldtm-stat-card.ldtm-accent-cyan > border { border-color:alpha(#31c8f4,0.48); }"
        ".ldtm-stat-card.ldtm-accent-purple > border { border-color:alpha(#b36cff,0.48); }"
        ".ldtm-stat-card.ldtm-accent-amber > border { border-color:alpha(#ffb52f,0.48); }"
        ".ldtm-stat-title { color:#91a7b8; font-size:9px; }"
        ".ldtm-stat-value { color:#f3f8fb; font-size:17px; font-weight:bold; }");

    /* The filesystem list is a visual matrix first; the spreadsheet is secondary. */
    g_string_append(
        css,
        ".ldtm-fs-tile > border { background-image:linear-gradient(135deg,#10171e,#0b1117);"
        " border:1px solid #293945; border-left-width:3px; border-radius:9px;"
        " box-shadow:inset 0 1px alpha(#ffffff,0.025); }"
        ".ldtm-fs-tile:hover > border { background-image:linear-gradient(135deg,#14212b,#0e171e); }"
        ".ldtm-fs-family-cyan > border { border-left-color:#27c4ef; }"
        ".ldtm-fs-family-green > border { border-left-color:#50d66f; }"
        ".ldtm-fs-family-purple > border { border-left-color:#9b62f2; }"
        ".ldtm-fs-family-magenta > border { border-left-color:#e05ad7; }"
        ".ldtm-fs-family-amber > border { border-left-color:#f5a524; }"
        ".ldtm-fs-family-neutral > border { border-left-color:#78a8c4; }"
        ".ldtm-fs-size { color:#7f95a5; font-size:9px; }"
        ".ldtm-fs-status { padding:2px 6px; border-radius:8px; font-size:9px; }"
        ".ldtm-fs-waiting .ldtm-fs-status { color:#8ea2b0; background-color:alpha(#8ea2b0,0.08); }"
        ".ldtm-fs-success .ldtm-fs-status { color:#68e487; background-color:alpha(#54db79,0.11); }"
        ".ldtm-fs-warning .ldtm-fs-status { color:#ffc45e; background-color:alpha(#ffb52f,0.12); }"
        ".ldtm-fs-failure .ldtm-fs-status { color:#ff7474; background-color:alpha(#ef5350,0.13); }"
        "flowboxchild { padding:0; background:transparent; }"
        "flowboxchild:selected { background:transparent; }");

    /* Large icon-led operations restore semantic colour and hierarchy. */
    g_string_append(
        css,
        ".ldtm-operation-button { min-height:76px; padding:0; border-radius:11px; color:#eef6fb;"
        " box-shadow:0 3px 10px alpha(#000000,0.28),inset 0 1px alpha(#ffffff,0.04); }"
        ".ldtm-operation-cyan { background-image:linear-gradient(120deg,#0b2633,#101a24); border:1px solid #168ab2; }"
        ".ldtm-operation-cyan:hover { background-image:linear-gradient(120deg,#104057,#142635); border-color:#32c7f3; }"
        ".ldtm-operation-purple { background-image:linear-gradient(120deg,#211832,#121925); border:1px solid #7447ac; }"
        ".ldtm-operation-purple:hover { background-image:linear-gradient(120deg,#342050,#171d2c); border-color:#b36cff; }"
        ".ldtm-operation-red { background-image:linear-gradient(120deg,#4a1719,#251317); border:1px solid #d34f52; }"
        ".ldtm-operation-red:hover { background-image:linear-gradient(120deg,#762528,#35161b); border-color:#ff6d70; }"
        ".ldtm-operation-title { color:#f4f8fb; font-size:12px; font-weight:bold; }"
        ".ldtm-operation-subtitle { color:#91a8b8; font-size:9px; }"
        ".ldtm-operation-button:disabled { opacity:1; background-image:linear-gradient(120deg,#10151a,#0d1115); border-color:#29323a; }"
        ".ldtm-operation-button:disabled .ldtm-operation-title { color:#75818a; }"
        ".ldtm-operation-button:disabled .ldtm-operation-subtitle { color:#56636d; }"
        ".ldtm-operation-button:disabled image { color:#5b6871; }"
        ".ldtm-operation-strip > border { background-image:linear-gradient(to right,#0d171e,#101421); border:1px solid #274455; border-radius:10px; }"
        "#ldtm-operation-strip-title { color:#8fb1c5; font-size:10px; font-weight:bold; }"
        "#ldtm-operation-summary { color:#d9eaf4; font-size:10px; }"
        "progressbar trough { min-height:12px; background-color:#081017; border:1px solid #263944; border-radius:7px; }"
        "progressbar progress { background-image:linear-gradient(to right,#00a9de,#5469f2,#a04bd7); border-radius:6px; }"
        "progressbar text { color:#e8f4fb; font-size:8pt; text-shadow:0 1px #000000; }");

    /* Technical table and raw log deliberately sit behind disclosure controls. */
    g_string_append_printf(
        css,
        ".ldtm-detail-expander,.ldtm-log-expander { color:#%06x; font-weight:bold; }"
        ".ldtm-detail-expander > title,.ldtm-log-expander > title { padding:6px 4px; }"
        "treeview,textview,textview text,viewport,scrolledwindow { background-color:#%06x; color:#%06x; border-color:#%06x; }"
        "treeview.view header button { background-image:linear-gradient(to bottom,#%06x,#%06x); border-color:#%06x; font-weight:bold; }"
        "treeview.view:selected { background-color:#%06x; color:#%06x; }"
        "scrollbar slider { background-color:#%06x; border-radius:%upx; min-width:7px; min-height:7px; }"
        "scrollbar slider:hover { background-color:#%06x; }"
        "tooltip { background-color:#%06x; color:#%06x; border:1px solid #%06x; }",
        (unsigned)palette->kicker_rgb,
        (unsigned)palette->surface_rgb,
        (unsigned)palette->text_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->card_rgb,
        (unsigned)palette->surface_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->selection_background_rgb,
        (unsigned)palette->selection_foreground_rgb,
        (unsigned)palette->note_rgb,
        metrics->small_radius,
        (unsigned)palette->status_border_rgb,
        (unsigned)palette->card_rgb,
        (unsigned)palette->text_rgb,
        (unsigned)palette->status_border_rgb);

    /* Dialogs keep a readable neutral escape and an unmistakable red destructive action. */
    g_string_append(
        css,
        "dialog button { background-image:linear-gradient(to bottom,#18222a,#11181e); color:#e7f0f5;"
        " border:1px solid #3b5262; border-radius:8px; box-shadow:none; text-shadow:none; opacity:1; }"
        "dialog button:hover { background-image:linear-gradient(to bottom,#213440,#17252e); border-color:#39b9e8; }"
        "dialog button:disabled { background-image:linear-gradient(to bottom,#11171c,#0c1115); color:#7c8b95; border-color:#29343b; opacity:1; }"
        "dialog button.ldtm-destructive-action { background-image:linear-gradient(to bottom,#7a2629,#4b171a); color:#ffffff; border-color:#ef6265; font-weight:bold; }"
        "dialog button.ldtm-destructive-action:hover { background-image:linear-gradient(to bottom,#9a3236,#5b1b1f); border-color:#ff8184; }"
        ".ldtm-danger-panel > border { background-image:linear-gradient(135deg,#351416,#171014); border:1px solid #a63b3f; border-radius:12px; }"
        ".ldtm-danger-icon-well { min-width:44px; min-height:44px; background-image:linear-gradient(to bottom right,#571a1d,#271114); border-color:#cf4d51; }"
        "separator { background-color:#263744; }");

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
