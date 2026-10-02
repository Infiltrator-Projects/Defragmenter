// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <infiltratr/design.h>

#include <gtk/gtk.h>

void ldtm_apply_mb_theme(void) {
    GdkScreen *screen = gdk_screen_get_default();
    GtkCssProvider *provider;
    char *css;
    char *action_css;
    char *combined_css;
    GError *error = NULL;
    const InfiltratrTypography *typography = infiltratr_typography();
    const InfiltratrDesignMetrics *metrics = infiltratr_design_metrics();
    const InfiltratrThemePalette *palette =
        infiltratr_theme_resolve(INFILTRATR_THEME_NIGHT, true);
    if (screen == NULL || typography == NULL || metrics == NULL || palette == NULL) return;

    /*
     * Common owns family identity and neutral structural metrics. Test Media
     * packages the verified faces and deliberately omits platform fallbacks.
     */
    css = g_strdup_printf(
        "* { font-family: \"%s\"; color: #%06x; }"
        "headerbar .title, .titlebar .title { font-family: \"%s\"; font-weight: normal; color: #%06x; }"
        "window, dialog, .background { background-color: #%06x; }"
        "headerbar, .titlebar { background-image: none; background-color: #%06x; border-bottom: 1px solid #%06x; color: #%06x; box-shadow: none; }"
        "headerbar label, .titlebar label { color: #%06x; font-weight: normal; }"
        "#ldtm-shell-header { min-height: 58px; padding: 6px 10px; background-image: linear-gradient(to right, #06131f, #08263a); background-color: #06131f; border-bottom: 1px solid #263746; }"
        "#ldtm-header-brand { padding: 2px 4px; }"
        "#ldtm-header-brand-icon { background-color: #111820; border: 1px solid #263746; border-radius: 12px; padding: 7px; box-shadow: 0 0 18px alpha(#00adef, 0.18); }"
        "#ldtm-header-brand-icon image { color: #00adef; }"
        "#ldtm-header-brand-title { color: #f4f7fa; font-size: 20px; font-weight: bold; }"
        "#ldtm-header-brand-subtitle { color: #9fb2c4; font-size: 11px; }"
        "#ldtm-header-end { margin-left: 10px; }"
        ".ldtm-window-control { min-width: 30px; min-height: 30px; padding: 4px; color: #d9e4ec; background-image: none; background-color: transparent; border: 1px solid transparent; border-radius: 8px; box-shadow: none; }"
        ".ldtm-window-control:hover { background-color: #163047; border-color: #365169; }"
        ".ldtm-window-control-close:hover { background-color: #c84343; border-color: #c84343; color: #ffffff; }"
        "menubar { background-color: #%06x; border-bottom: 1px solid #%06x; padding: 4px 8px; }"
        "menu { background-color: #%06x; border: 1px solid #%06x; }"
        "menuitem { padding: 7px 11px; }"
        "menuitem:hover { background-color: #%06x; }"
        "frame > border { background-color: #%06x; border: 1px solid #%06x; border-radius: %upx; }"
        "button { background-image: none; background-color: #%06x; color: #%06x; border: 1px solid #%06x; border-radius: %upx; padding: 7px 13px; min-height: 27px; box-shadow: none; text-shadow: none; }"
        "button:hover { background-color: #%06x; border-color: #%06x; }"
        "button:active, button:checked { background-color: #%06x; border-color: #%06x; }"
        "button:disabled { color: #%06x; border-color: #%06x; background-color: #%06x; }"
        "entry, combobox button, spinbutton { background-image: none; background-color: #%06x; color: #%06x; border: 1px solid #%06x; border-radius: %upx; box-shadow: none; min-height: 28px; }"
        "entry:focus, spinbutton:focus { border-color: #%06x; background-color: #%06x; }"
        "textview, textview text, treeview, viewport, scrolledwindow { background-color: #%06x; color: #%06x; border-color: #%06x; }"
        "entry selection, textview text selection, treeview.view:selected { background-color: #%06x; color: #%06x; }"
        "treeview.view header button { background-color: #%06x; border-color: #%06x; font-weight: bold; }"
        "notebook > header { background-color: #%06x; border-color: #%06x; }"
        "notebook tab { background-color: #%06x; padding: 7px 12px; }"
        "notebook tab:checked { background-color: #%06x; }"
        "progressbar trough { min-height: 8px; background-color: #%06x; border: 1px solid #%06x; border-radius: %upx; }"
        "progressbar progress { background-color: #%06x; border-radius: %upx; }"
        "progressbar text { color: #%06x; font-size: 8pt; }"
        "scrollbar slider { background-color: #%06x; border-radius: %upx; min-width: 7px; min-height: 7px; }"
        "scrollbar slider:hover { background-color: #%06x; }"
        "separator { background-color: #%06x; }"
        "tooltip { background-color: #%06x; color: #%06x; border: 1px solid #%06x; }",
        typography->ui_family, (unsigned)palette->text_rgb,
        typography->brand_family, (unsigned)palette->heading_rgb,
        (unsigned)palette->background_rgb,
        (unsigned)palette->titlebar_rgb, (unsigned)palette->status_border_rgb, (unsigned)palette->heading_rgb,
        (unsigned)palette->summary_rgb,
        (unsigned)palette->connection_rgb, (unsigned)palette->connection_border_rgb,
        (unsigned)palette->card_rgb, (unsigned)palette->border_rgb,
        (unsigned)palette->card_hover_rgb,
        (unsigned)palette->background_rgb, (unsigned)palette->border_rgb, metrics->small_radius,
        (unsigned)palette->button_background_rgb, (unsigned)palette->button_foreground_rgb,
        (unsigned)palette->border_rgb, metrics->control_radius,
        (unsigned)palette->surface_hover_rgb, (unsigned)palette->accent_hover_rgb,
        (unsigned)palette->operation_hover_rgb, (unsigned)palette->accent_hover_rgb,
        (unsigned)palette->subtle_rgb, (unsigned)palette->border_rgb, (unsigned)palette->background_rgb,
        (unsigned)palette->input_rgb, (unsigned)palette->text_rgb, (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->subtle_rgb, (unsigned)palette->surface_hover_rgb,
        (unsigned)palette->surface_rgb, (unsigned)palette->text_rgb, (unsigned)palette->border_rgb,
        (unsigned)palette->selection_background_rgb, (unsigned)palette->selection_foreground_rgb,
        (unsigned)palette->card_rgb, (unsigned)palette->border_rgb,
        (unsigned)palette->background_rgb, (unsigned)palette->border_rgb,
        (unsigned)palette->surface_rgb, (unsigned)palette->operation_hover_rgb,
        (unsigned)palette->background_rgb, (unsigned)palette->border_rgb, metrics->small_radius,
        (unsigned)palette->neutral_accent_rgb, metrics->small_radius,
        (unsigned)palette->detail_label_rgb,
        (unsigned)palette->note_rgb, metrics->small_radius,
        (unsigned)palette->text_rgb,
        (unsigned)palette->status_border_rgb,
        (unsigned)palette->card_rgb, (unsigned)palette->note_rgb, (unsigned)palette->status_border_rgb);

    action_css = g_strdup_printf(
        "button.ldtm-primary-action { background-image: none; background-color: #%06x; color: #%06x; border-color: #%06x; font-weight: bold; box-shadow: none; text-shadow: none; opacity: 1; }"
        "button.ldtm-primary-action:hover { background-image: none; background-color: #%06x; border-color: #%06x; }"
        "button.ldtm-primary-action:disabled { background-image: none; background-color: #%06x; color: #%06x; border-color: #%06x; box-shadow: none; text-shadow: none; opacity: 1; }"
        "button.ldtm-destructive-action { background-image: none; background-color: #%06x; color: #%06x; border-color: #%06x; font-weight: bold; box-shadow: none; text-shadow: none; opacity: 1; }"
        "button.ldtm-destructive-action:hover { background-image: none; background-color: #%06x; border-color: #%06x; }"
        "button.ldtm-destructive-action:disabled { background-image: none; background-color: #%06x; color: #%06x; border-color: #%06x; box-shadow: none; text-shadow: none; opacity: 1; }",
        (unsigned)palette->card_rgb, (unsigned)palette->text_rgb,
        (unsigned)palette->neutral_accent_rgb,
        (unsigned)palette->surface_hover_rgb, (unsigned)palette->accent_hover_rgb,
        (unsigned)palette->background_rgb, (unsigned)palette->subtle_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->card_rgb, (unsigned)palette->text_rgb,
        (unsigned)palette->fault_rgb,
        (unsigned)palette->card_hover_rgb, (unsigned)palette->fault_rgb,
        (unsigned)palette->background_rgb, (unsigned)palette->subtle_rgb,
        (unsigned)palette->border_rgb);
    combined_css = g_strconcat(css, action_css, NULL);
    g_free(action_css);
    g_free(css);
    css = combined_css;

    provider = gtk_css_provider_new();
    if (gtk_css_provider_load_from_data(provider, css, -1, &error)) {
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 50);
    }
    if (error != NULL) g_error_free(error);
    g_object_unref(provider);
    g_free(css);
}
