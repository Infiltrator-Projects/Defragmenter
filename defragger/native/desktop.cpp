// SPDX-License-Identifier: GPL-3.0-or-later
// Native GTK desktop client. Filesystem parsing and writes stay in the native engines.
#include "desktop_policy.hpp"
#include "desktop_live_map.hpp"
#include "process.hpp"

extern "C" {
#include "version.h"
#include <infiltratr/design.h>
}

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using defragger::DesktopVolume;
using defragger::Json;
namespace fs = std::filesystem;

namespace {
const char* field(const Json& data, const char* key) {
    const auto* item = data.find(key);
    return item && item->is_string() ? item->string().data() : "";
}
std::uint64_t number(const Json& data, const char* key) {
    const auto* item = data.find(key);
    return item ? item->unsigned_or() : 0;
}
double real_number(const Json& data, const char* key, double fallback = 0.0) {
    const auto* item = data.find(key);
    return item ? item->real_or(fallback) : fallback;
}
std::string bytes(std::uint64_t value) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double amount = static_cast<double>(value);
    int index = 0;
    while (amount >= 1024.0 && index < 4) { amount /= 1024.0; ++index; }
    char text[64];
    g_snprintf(text, sizeof(text), "%.1f %s", amount, units[index]);
    return text;
}
GtkWidget* section(const char* title, GtkWidget* child) {
    auto* frame = gtk_frame_new(title);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "panel");
    gtk_container_set_border_width(GTK_CONTAINER(child), 12);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}
void css_class(GtkWidget* widget, const char* name) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}
fs::path artwork(const char* name) {
    const fs::path installed = fs::path("/usr/lib/linux-defragger/ui/art") / name;
    if (fs::is_regular_file(installed)) return installed;
    const auto local = fs::path("defragger/gui/ui/art") / name;
    if (fs::is_regular_file(local)) return local;
    return {};
}

GtkWidget* app_icon_image(int size)
{
    const fs::path candidates[] = {
        "/usr/lib/linux-defragger/defragmenter-icon.png",
        "/usr/share/icons/hicolor/96x96/apps/io.github.linuxdefragger.png",
        fs::path("defragger/packaging/io.github.linuxdefragger.png"),
        fs::path("packaging/io.github.linuxdefragger.png"),
    };
    for (const auto& path : candidates) {
        if (!fs::is_regular_file(path)) continue;
        GError* failure = nullptr;
        auto* pixbuf = gdk_pixbuf_new_from_file_at_scale(
            path.string().c_str(), size, size, TRUE, &failure);
        if (failure != nullptr) g_error_free(failure);
        if (pixbuf == nullptr) continue;
        auto* image = gtk_image_new_from_pixbuf(pixbuf);
        g_object_unref(pixbuf);
        return image;
    }
    auto* image = gtk_image_new_from_icon_name(
        "io.github.linuxdefragger", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    return image;
}

void apply_window_icon(GtkWindow* window)
{
    const fs::path candidates[] = {
        "/usr/lib/linux-defragger/defragmenter-icon.png",
        "/usr/share/icons/hicolor/96x96/apps/io.github.linuxdefragger.png",
        fs::path("defragger/packaging/io.github.linuxdefragger.png"),
        fs::path("packaging/io.github.linuxdefragger.png"),
    };
    for (const auto& path : candidates) {
        if (!fs::is_regular_file(path)) continue;
        GError* failure = nullptr;
        auto* pixbuf = gdk_pixbuf_new_from_file(path.string().c_str(), &failure);
        if (failure != nullptr) g_error_free(failure);
        if (pixbuf == nullptr) continue;
        gtk_window_set_icon(window, pixbuf);
        g_object_unref(pixbuf);
        return;
    }
    gtk_window_set_icon_name(window, "io.github.linuxdefragger");
}

gboolean draw_hero_art(GtkWidget* widget, cairo_t* cr, gpointer) {
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const int width = std::max(1, allocation.width);
    const int height = std::max(1, allocation.height);

    cairo_set_source_rgb(cr, 0.018, 0.028, 0.045);
    cairo_paint(cr);

    auto* source = static_cast<GdkPixbuf*>(
        g_object_get_data(G_OBJECT(widget), "hero-source"));
    if (source == nullptr) {
        const fs::path path = artwork("hero-landscape.jpg");
        if (!path.empty()) {
            GError* failure = nullptr;
            source = gdk_pixbuf_new_from_file(path.string().c_str(), &failure);
            if (failure != nullptr) g_error_free(failure);
            if (source != nullptr)
                g_object_set_data_full(
                    G_OBJECT(widget), "hero-source", source,
                    reinterpret_cast<GDestroyNotify>(g_object_unref));
        }
    }

    if (source != nullptr) {
        const int destination_width = std::max(1, width * 68 / 100);
        const int source_width = std::max(1, gdk_pixbuf_get_width(source));
        const int source_height = std::max(1, gdk_pixbuf_get_height(source));
        const double scale = std::max(
            static_cast<double>(destination_width) / source_width,
            static_cast<double>(height) / source_height);
        const int scaled_width =
            std::max(1, static_cast<int>(source_width * scale + 0.5));
        const int scaled_height =
            std::max(1, static_cast<int>(source_height * scale + 0.5));
        auto* scaled = gdk_pixbuf_scale_simple(
            source, scaled_width, scaled_height, GDK_INTERP_BILINEAR);
        if (scaled != nullptr) {
            const double x =
                width - destination_width +
                (destination_width - scaled_width) / 2.0;
            const double y = (height - scaled_height) / 2.0;
            cairo_save(cr);
            cairo_rectangle(
                cr, width - destination_width, 0,
                destination_width, height);
            cairo_clip(cr);
            gdk_cairo_set_source_pixbuf(cr, scaled, x, y);
            cairo_paint(cr);
            cairo_restore(cr);
            g_object_unref(scaled);
        }
    }

    // A subtle left-to-right veil makes selected-volume text readable without
    // muting the landscape on the right.
    cairo_pattern_t* veil =
        cairo_pattern_create_linear(0.0, 0.0, width * 0.78, 0.0);
    cairo_pattern_add_color_stop_rgba(
        veil, 0.0, 0.018, 0.028, 0.045, 1.0);
    cairo_pattern_add_color_stop_rgba(
        veil, 0.52, 0.018, 0.028, 0.045, 0.88);
    cairo_pattern_add_color_stop_rgba(
        veil, 1.0, 0.018, 0.028, 0.045, 0.0);
    cairo_set_source(cr, veil);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
    cairo_pattern_destroy(veil);
    return FALSE;
}

gboolean draw_raster_cover(
    GtkWidget* widget, cairo_t* cr, const char* asset_name,
    const char* cache_key)
{
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const int width = std::max(1, allocation.width);
    const int height = std::max(1, allocation.height);

    auto* source = static_cast<GdkPixbuf*>(
        g_object_get_data(G_OBJECT(widget), cache_key));
    if (source == nullptr) {
        const fs::path path = artwork(asset_name);
        if (!path.empty()) {
            GError* failure = nullptr;
            source = gdk_pixbuf_new_from_file(path.string().c_str(), &failure);
            if (failure != nullptr) g_error_free(failure);
            if (source != nullptr)
                g_object_set_data_full(
                    G_OBJECT(widget), cache_key, source,
                    reinterpret_cast<GDestroyNotify>(g_object_unref));
        }
    }
    if (source == nullptr) return FALSE;

    const int source_width = std::max(1, gdk_pixbuf_get_width(source));
    const int source_height = std::max(1, gdk_pixbuf_get_height(source));
    const double scale = std::max(
        static_cast<double>(width) / source_width,
        static_cast<double>(height) / source_height);
    const int scaled_width =
        std::max(1, static_cast<int>(source_width * scale + 0.5));
    const int scaled_height =
        std::max(1, static_cast<int>(source_height * scale + 0.5));
    auto* scaled = gdk_pixbuf_scale_simple(
        source, scaled_width, scaled_height, GDK_INTERP_BILINEAR);
    if (scaled == nullptr) return FALSE;

    const double x = (width - scaled_width) / 2.0;
    const double y = (height - scaled_height) / 2.0;
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    gdk_cairo_set_source_pixbuf(cr, scaled, x, y);
    cairo_paint(cr);
    cairo_restore(cr);
    g_object_unref(scaled);
    return FALSE;
}

gboolean draw_drive_art(GtkWidget* widget, cairo_t* cr, gpointer)
{
    return draw_raster_cover(
        widget, cr, "drive-ssd.jpg", "drive-art-source");
}

gboolean draw_sidebar_art(GtkWidget* widget, cairo_t* cr, gpointer)
{
    return draw_raster_cover(
        widget, cr, "sidebar-workbench.jpg", "sidebar-art-source");
}

void fit_window_to_workarea(GtkWidget* widget, gpointer)
{
    auto* native = gtk_widget_get_window(widget);
    auto* display = gdk_display_get_default();
    if (native == nullptr || display == nullptr) return;
    auto* monitor = gdk_display_get_monitor_at_window(display, native);
    if (monitor == nullptr) return;

    GdkRectangle workarea{};
    gdk_monitor_get_workarea(monitor, &workarea);
    const int width = std::min(1480, std::max(900, workarea.width - 48));
    const int height = std::min(900, std::max(620, workarea.height - 64));
    gtk_window_resize(GTK_WINDOW(widget), width, height);
}

GtkCssProvider* style_provider = nullptr;

std::string rgb_hex(std::uint32_t rgb) {
    char text[8];
    std::snprintf(text, sizeof(text), "#%06X", rgb & 0xFFFFFFU);
    return text;
}

bool system_prefers_dark() {
    GtkSettings* settings = gtk_settings_get_default();
    if (settings == nullptr) return false;

    gboolean prefer_dark = FALSE;
    if (g_object_class_find_property(
            G_OBJECT_GET_CLASS(settings),
            "gtk-application-prefer-dark-theme") != nullptr) {
        g_object_get(settings, "gtk-application-prefer-dark-theme",
                     &prefer_dark, nullptr);
        if (prefer_dark) return true;
    }

    gchar* theme_name = nullptr;
    if (g_object_class_find_property(
            G_OBJECT_GET_CLASS(settings), "gtk-theme-name") != nullptr) {
        g_object_get(settings, "gtk-theme-name", &theme_name, nullptr);
    }
    std::string name = theme_name ? theme_name : "";
    g_free(theme_name);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return name.find("dark") != std::string::npos;
}

std::string theme_file() {
    return (fs::path(g_get_user_config_dir()) /
            "linux-defragger" / "theme").string();
}

InfiltratrThemeMode load_theme_mode() {
    const std::string path = theme_file();
    gchar* raw = nullptr;
    gsize length = 0U;
    if (!g_file_get_contents(path.c_str(), &raw, &length, nullptr))
        return INFILTRATR_THEME_SYSTEM;
    std::string value(raw, length);
    g_free(raw);
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' ||
            value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
    InfiltratrThemeMode mode = INFILTRATR_THEME_SYSTEM;
    return infiltratr_theme_mode_parse(value.c_str(), &mode)
        ? mode : INFILTRATR_THEME_SYSTEM;
}

void save_theme_mode(InfiltratrThemeMode mode) {
    const fs::path path(theme_file());
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return;
    const std::string value =
        std::string(infiltratr_theme_mode_key(mode)) + "\n";
    (void)g_file_set_contents(path.c_str(), value.c_str(),
                              static_cast<gssize>(value.size()), nullptr);
}

const char* theme_label(InfiltratrThemeMode mode) {
    switch (mode) {
        case INFILTRATR_THEME_DAY: return "Appearance: Day";
        case INFILTRATR_THEME_NIGHT: return "Appearance: Night";
        case INFILTRATR_THEME_SYSTEM:
        default: return "Appearance: Follow system";
    }
}

const InfiltratrThemePalette* install_style(InfiltratrThemeMode mode) {
    const auto* palette =
        infiltratr_theme_resolve(mode, system_prefers_dark());
    const auto* day_palette =
        infiltratr_theme_resolve(INFILTRATR_THEME_DAY, false);
    const auto* chrome =
        infiltratr_theme_resolve(INFILTRATR_THEME_NIGHT, true);
    const auto* typography = infiltratr_typography();
    const auto* metrics = infiltratr_design_metrics();
    if (palette == nullptr || day_palette == nullptr || chrome == nullptr ||
        typography == nullptr || metrics == nullptr)
        throw std::runtime_error("Common design contract is unavailable");

    const bool day_mode =
        palette->background_rgb == day_palette->background_rgb &&
        palette->text_rgb == day_palette->text_rgb;

    const std::string background = rgb_hex(palette->background_rgb);
    const std::string panel = rgb_hex(palette->panel_rgb);
    const std::string card = rgb_hex(palette->card_rgb);
    const std::string surface = rgb_hex(palette->surface_rgb);
    const std::string input = rgb_hex(palette->input_rgb);
    const std::string border = rgb_hex(palette->border_rgb);
    const std::string text = rgb_hex(palette->text_rgb);
    const std::string heading = rgb_hex(palette->heading_rgb);
    const std::string detail = rgb_hex(palette->detail_label_rgb);
    const std::string kicker = rgb_hex(palette->kicker_rgb);
    const std::string accent = rgb_hex(palette->neutral_accent_rgb);
    const std::string accent_hover = rgb_hex(palette->accent_hover_rgb);
    const std::string button_bg = rgb_hex(palette->button_background_rgb);
    const std::string button_fg = rgb_hex(palette->button_foreground_rgb);
    const std::string fault = rgb_hex(palette->fault_rgb);
    const std::string success = rgb_hex(palette->success_rgb);
    const std::string info = rgb_hex(palette->info_rgb);
    const std::string summary = rgb_hex(palette->summary_rgb);
    const std::string note = rgb_hex(palette->note_rgb);
    const std::string status_border = rgb_hex(palette->status_border_rgb);
    const std::string operation = rgb_hex(palette->operation_rgb);

    const std::string chrome_titlebar = rgb_hex(chrome->titlebar_rgb);
    const std::string chrome_heading = rgb_hex(chrome->heading_rgb);
    const std::string chrome_summary = rgb_hex(chrome->summary_rgb);
    const std::string chrome_detail = rgb_hex(chrome->detail_label_rgb);
    const std::string chrome_border = rgb_hex(chrome->status_border_rgb);
    const std::string chrome_hover = rgb_hex(chrome->surface_hover_rgb);
    const std::string chrome_accent = rgb_hex(chrome->neutral_accent_rgb);

    const std::string shell_background = day_mode ? surface : background;
    const std::string sidebar_background = day_mode ? panel : surface;
    const std::string card_background = day_mode ? panel : card;
    const std::string control_border = day_mode ? border : accent;
    const std::string text_background = day_mode ? panel : input;

    std::string css;
    css.reserve(6144U);
    css += "window { background: " + background + "; color: " + text +
           "; font-family: '" + typography->ui_family + "'; }\n";
    css += ".app-shell { background: " + shell_background + "; color: " +
           text + "; font-family: '" + typography->ui_family + "'; }\n";

    /*
     * The suite title bar and hero artwork are dark branded surfaces in both
     * appearance modes.  Use Common's Night semantic roles for text/chrome on
     * those dark rasters instead of placing Day's dark text on dark imagery.
     */
    css += "headerbar { background: " + chrome_titlebar + "; color: " +
           chrome_heading + "; border-bottom: 1px solid " + chrome_border +
           "; min-height: 58px; padding: 2px 8px; }\n";
    css += "headerbar button { background: transparent; border: 0; color: " +
           chrome_heading + "; box-shadow: none; padding: 2px; }\n";
    css += ".header-end { margin-left: 10px; }\n";
    css += ".header-control { min-width: 30px; min-height: 30px; padding: 4px; background: transparent; border: 1px solid transparent; border-radius: 8px; }\n";
    css += ".header-control:hover { background: " + chrome_hover + "; border-color: " + chrome_border + "; }\n";
    css += ".header-control-close:hover { background: " + fault + "; color: " + button_fg + "; }\n";
    css += ".brand-title { font-family: '" +
           std::string(typography->brand_family) +
           "'; font-size: 20px; font-weight: 600; color: " +
           chrome_heading + "; }\n";
    css += ".brand-subtitle { font-size: 10px; letter-spacing: 2px; color: " +
           chrome_summary + "; }\n";

    css += ".sidebar { background-image: linear-gradient(to bottom, " +
           sidebar_background + ", " + background +
           "); border-right: 1px solid " + status_border + "; }\n";
    css += ".sidebar-brand-title { font-family: '" +
           std::string(typography->brand_family) +
           "'; font-size: 20px; font-weight: 600; color: " + heading + "; }\n";
    css += ".sidebar-brand-subtitle { font-size: 11px; color: " + detail + "; }\n";
    css += ".nav-button { background: transparent; color: " + detail +
           "; border: 1px solid transparent; padding: 4px 7px; min-height: 44px; border-radius: " +
           std::to_string(metrics->card_radius) + "px; }\n";
    css += ".nav-button:hover { background: " +
           rgb_hex(palette->card_hover_rgb) + "; color: " + heading +
           "; border-color: " + border + "; }\n";
    css += ".nav-selected { background-image: linear-gradient(90deg, alpha(" +
           accent + ", 0.22), alpha(" + info +
           ", 0.08)); color: " + heading +
           "; border-color: " + accent + "; }\n";
    css += ".nav-title { font-size: 13px; font-weight: 700; color: " + heading + "; }\n";
    css += ".nav-subtitle { font-size: 9px; color: " + detail + "; }\n";
    css += ".nav-icon-overview { color: " + accent + "; }\n";
    css += ".nav-icon-analyse { color: " + accent + "; }\n";
    css += ".nav-icon-defrag { color: " + info + "; }\n";
    css += ".nav-icon-growth { color: " + success + "; }\n";
    css += ".nav-icon-recover { color: #9B5CFF; }\n";
    css += ".nav-icon-test-media { color: " + success + "; }\n";
    css += ".nav-icon-settings { color: " + summary + "; }\n";
    css += ".card { background: " + card_background +
           "; border: 1px solid " + border + "; border-radius: " +
           std::to_string(metrics->card_radius) + "px; }\n";
    css += "frame.panel > border { background: " + card_background +
           "; border: 1px solid " + border + "; border-radius: " +
           std::to_string(metrics->panel_radius) + "px; }\n";
    css += ".summary-fragmented { border-color: " + fault + "; }\n";
    css += ".summary-free { border-color: " + accent + "; }\n";
    css += ".summary-capacity { border-color: " + info + "; }\n";
    css += ".summary-files { border-color: " + success + "; }\n";
    css += "frame.volume-selector-panel > border { border-color: " +
           status_border + "; }\n";
    css += "frame.map-panel > border { border-color: " + accent +
           "; background-image: linear-gradient(to bottom, " +
           card_background + ", " + shell_background + "); }\n";
    css += "frame.activity-panel > border { border-color: " +
           status_border + "; }\n";
    css += ".hero { background: #07101B; border: 1px solid " + border +
           "; border-radius: " + std::to_string(metrics->panel_radius) +
           "px; }\n";
    css += ".hero-drive { background: rgba(5, 10, 16, 0.72); border: 1px solid " +
           chrome_accent + "; border-radius: 10px; padding: 8px; }\n";
    css += ".version-badge { background: rgba(5, 10, 16, 0.78); border: 1px solid " +
           chrome_border + "; border-radius: 8px; padding: 6px 9px; color: " +
           chrome_summary + "; }\n";
    css += ".hero-status { color: " + success +
           "; background: rgba(5, 10, 16, 0.78); border: 1px solid " +
           chrome_border + "; border-radius: 7px; padding: 5px 8px; }\n";
    css += ".hero-title { font-family: '" +
           std::string(typography->brand_family) +
           "'; font-size: 23px; font-weight: 600; color: " +
           chrome_heading + "; }\n";
    css += ".hero .hint { color: " + chrome_detail + "; }\n";
    css += ".hero .kicker { color: " +
           (day_mode ? chrome_accent : kicker) +
           "; font-size: 11px; letter-spacing: 2px; }\n";
    css += ".hint { color: " + detail + "; }\n";
    css += ".kicker { color: " + kicker +
           "; font-size: 11px; letter-spacing: 2px; }\n";
    css += ".summary-value { color: " + heading + "; font-size: 26px; font-weight: 700; }\n";
    css += ".summary-detail { color: " + detail + "; font-size: 8px; }\n";
    css += ".panel { background: " + card_background + "; border: 1px solid " +
           border + "; border-radius: " +
           std::to_string(metrics->card_radius) + "px; }\n";
    css += ".action-card { min-height: 86px; padding: 6px; background: " +
           card_background + "; border: 1px solid " + border + "; border-radius: " +
           std::to_string(metrics->card_radius) + "px; }\n";
    css += ".action-card:hover { border-color: " + accent_hover + "; }\n";
    css += ".action-analyse { border-color: " + accent + "; }\n";
    css += ".action-defrag { border-color: " + info + "; }\n";
    css += ".action-growth { border-color: " + success + "; }\n";
    css += ".action-recover { border-color: #8F52FF; }\n";
    css += ".action-analyse image, .action-analyse .action-arrow { color: " + accent + "; }\n";
    css += ".action-defrag image, .action-defrag .action-arrow { color: " + info + "; }\n";
    css += ".action-growth image, .action-growth .action-arrow { color: " + success + "; }\n";
    css += ".action-recover image, .action-recover .action-arrow { color: #A979FF; }\n";
    css += ".action-arrow { font-size: 22px; font-weight: 700; padding: 0 5px; }\n";
    css += ".action-title { font-size: 15px; font-weight: 700; color: " + heading + "; }\n";
    css += ".action-subtitle { font-size: 9px; color: " + detail + "; }\n";
    css += ".activity-primary { font-weight: 700; color: " + heading + "; }\n";
    css += ".activity-secondary { color: " + detail + "; font-size: 9px; }\n";
    css += ".status-strip { background-image: linear-gradient(90deg, " +
           rgb_hex(palette->connection_rgb) + ", " + surface +
           "); border-top: 1px solid " + status_border + "; }\n";
    css += ".ready-dot { color: " + success + "; }\n";
    css += ".footer-volume { color: " + heading + "; font-weight: 700; }\n";
    css += ".status-text { color: " + note + "; }\n";
    css += ".page-title { font-family: '" + std::string(typography->brand_family) +
           "'; font-size: 32px; font-weight: 600; color: " + heading + "; }\n";
    css += ".page-subtitle, .page-volume { color: " + detail + "; }\n";
    css += ".operation-page { background: " + shell_background + "; }\n";
    css += ".operation-page .panel { background: " + card_background + "; }\n";
    css += ".gauge-title { color: " + detail + "; font-size: 10px; letter-spacing: 1px; }\n";

    css += "button { border-radius: " +
           std::to_string(metrics->control_radius) +
           "px; padding: 7px 12px; background: " + card_background +
           "; color: " + text + "; border: 1px solid " + control_border +
           "; box-shadow: none; }\n";
    css += "button:hover { background: " +
           rgb_hex(palette->card_hover_rgb) + "; border-color: " +
           accent_hover + "; }\n";
    css += ".primary-action { background: " + button_bg + "; color: " +
           button_fg + "; border-color: " + button_bg +
           "; font-weight: bold; }\n";
    css += ".stop-action { background: " + operation + "; color: " + text +
           "; border-color: " + fault + "; }\n";

    css += "combobox button, entry, spinbutton { background: " +
           text_background + "; color: " + text + "; border: 1px solid " +
           border + "; }\n";
    css += "textview, textview text { background: " + text_background +
           "; color: " + text + "; }\n";
    css += "progressbar trough { background: " +
           (day_mode ? card : panel) +
           "; border: 1px solid " + border + "; border-radius: " +
           std::to_string(metrics->small_radius) + "px; min-height: 8px; }\n";
    css += "progressbar progress { background: " + accent +
           "; border-radius: " + std::to_string(metrics->small_radius) +
           "px; }\n";

    if (style_provider == nullptr) {
        style_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(style_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 50);
    }
    GError* failure = nullptr;
    if (!gtk_css_provider_load_from_data(
            style_provider, css.c_str(), static_cast<gssize>(css.size()),
            &failure)) {
        std::string message =
            failure ? failure->message : "unknown CSS parser error";
        if (failure) g_error_free(failure);
        throw std::runtime_error("Unable to apply Common theme: " + message);
    }
    return palette;
}


class Desktop {
public:
    Desktop() {
        mapper_ = defragger::resolve_program("mapper");
        engine_ = defragger::resolve_program("operation-engine");
        helper_path_ = defragger::resolve_program("helper");
        theme_mode_ = load_theme_mode();
        palette_ = install_style(theme_mode_);
        map_palette_ = infiltratr_theme_resolve(INFILTRATR_THEME_NIGHT, true);
        if (map_palette_ == nullptr)
            throw std::runtime_error("Common map palette is unavailable");
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        g_object_set_data(G_OBJECT(window_), "desktop", this);
        gtk_window_set_title(GTK_WINDOW(window_), "Defragmenter");
        gtk_window_set_default_size(GTK_WINDOW(window_), 1480, 900);
        gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
        gtk_widget_set_size_request(window_, 900, 620);
        g_signal_connect(
            window_, "realize", G_CALLBACK(fit_window_to_workarea), nullptr);
        apply_window_icon(GTK_WINDOW(window_));
        g_signal_connect(window_, "delete-event", G_CALLBACK(close_requested), this);
        g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {
            gtk_main_quit();
        }), nullptr);
        auto* header = gtk_header_bar_new();
        gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), FALSE);
        gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header), gtk_label_new(""));
        gtk_widget_set_size_request(header, -1, 58);
        auto* brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
        auto* icon = app_icon_image(32);
        gtk_box_pack_start(GTK_BOX(brand), icon, FALSE, FALSE, 0);
        auto* brand_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        auto* brand_name = gtk_label_new("Defragmenter");
        css_class(brand_name, "brand-title");
        gtk_label_set_xalign(GTK_LABEL(brand_name), 0);
        auto* suite = gtk_label_new("INFILTRATOR OS");
        css_class(suite, "brand-subtitle");
        gtk_label_set_xalign(GTK_LABEL(suite), 0);
        gtk_box_pack_start(GTK_BOX(brand_text), brand_name, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(brand_text), suite, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(brand), brand_text, FALSE, FALSE, 0);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(header), brand);

        /*
         * Match System Settings: pack one trailing box so GTK cannot reverse
         * the individual controls. The visible order is always
         * Minimize | Maximize | Close, with Close at the far right.
         */
        auto* header_end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        css_class(header_end, "header-end");

        auto* minimize = gtk_button_new_from_icon_name(
            "window-minimize-symbolic", GTK_ICON_SIZE_BUTTON);
        auto* maximize = gtk_button_new_from_icon_name(
            "window-maximize-symbolic", GTK_ICON_SIZE_BUTTON);
        auto* close = gtk_button_new_from_icon_name(
            "window-close-symbolic", GTK_ICON_SIZE_BUTTON);
        for (auto* button : {minimize, maximize, close}) {
            css_class(button, "header-control");
            gtk_widget_set_size_request(button, 32, 32);
        }
        css_class(close, "header-control-close");

        g_object_set_data_full(
            G_OBJECT(minimize), "action", g_strdup("minimize"), g_free);
        g_object_set_data_full(
            G_OBJECT(maximize), "action", g_strdup("maximize"), g_free);
        g_object_set_data_full(
            G_OBJECT(close), "action", g_strdup("close"), g_free);
        g_signal_connect(minimize, "clicked", G_CALLBACK(clicked), nullptr);
        g_signal_connect(maximize, "clicked", G_CALLBACK(clicked), nullptr);
        g_signal_connect(close, "clicked", G_CALLBACK(clicked), nullptr);

        gtk_box_pack_start(
            GTK_BOX(header_end), minimize, FALSE, FALSE, 0);
        gtk_box_pack_start(
            GTK_BOX(header_end), maximize, FALSE, FALSE, 0);
        gtk_box_pack_start(
            GTK_BOX(header_end), close, FALSE, FALSE, 0);
        gtk_header_bar_pack_end(GTK_HEADER_BAR(header), header_end);
        gtk_window_set_titlebar(GTK_WINDOW(window_), header);

        auto* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_add(GTK_CONTAINER(window_), outer);

        auto* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start(GTK_BOX(outer), paned, TRUE, TRUE, 0);
        auto* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        css_class(sidebar, "sidebar");
        gtk_container_set_border_width(GTK_CONTAINER(sidebar), 12);
        gtk_widget_set_size_request(sidebar, 238, -1);
        gtk_paned_pack1(GTK_PANED(paned), sidebar, FALSE, FALSE);
        gtk_paned_set_position(GTK_PANED(paned), 238);

        auto* sidebar_brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
        auto* sidebar_icon = app_icon_image(52);
        gtk_box_pack_start(
            GTK_BOX(sidebar_brand), sidebar_icon, FALSE, FALSE, 0);
        auto* sidebar_copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        auto* sidebar_title = gtk_label_new("Defragmenter");
        css_class(sidebar_title, "sidebar-brand-title");
        gtk_label_set_xalign(GTK_LABEL(sidebar_title), 0);
        auto* sidebar_subtitle = gtk_label_new("Visual disk optimisation");
        css_class(sidebar_subtitle, "sidebar-brand-subtitle");
        gtk_label_set_xalign(GTK_LABEL(sidebar_subtitle), 0);
        gtk_box_pack_start(
            GTK_BOX(sidebar_copy), sidebar_title, FALSE, FALSE, 0);
        gtk_box_pack_start(
            GTK_BOX(sidebar_copy), sidebar_subtitle, FALSE, FALSE, 0);
        gtk_box_pack_start(
            GTK_BOX(sidebar_brand), sidebar_copy, TRUE, TRUE, 0);
        gtk_box_pack_start(
            GTK_BOX(sidebar), sidebar_brand, FALSE, FALSE, 5);

        nav_overview_ = add_nav_button(
            sidebar, "go-home-symbolic", "Overview", "Drive at a glance",
            "page-overview", true);
        nav_analyse_ = add_nav_button(
            sidebar, "system-search-symbolic", "Analyse", "Scan and visualise",
            "page-analyse", false);
        nav_defrag_ = add_nav_button(
            sidebar, "view-grid-symbolic", "Defragment", "Optimise file layout",
            "page-defrag", false);
        nav_growth_ = add_nav_button(
            sidebar, "go-up-symbolic", "Growth Defrag",
            "Keep free space contiguous", "page-growth", false);
        nav_recover_ = add_nav_button(
            sidebar, "edit-undo-symbolic", "Recover", "Resume safe recovery",
            "page-recover", false);

        auto* nav_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start(GTK_BOX(sidebar), nav_separator, FALSE, FALSE, 5);

        nav_test_media_ = add_nav_button(
            sidebar, "drive-removable-media-symbolic", "Test Media",
            "Check and diagnose", "page-test-media", false);
        nav_settings_ = add_nav_button(
            sidebar, "preferences-system-symbolic", "Settings",
            "Appearance and preferences", "page-settings", false);

        auto* sidebar_art = gtk_drawing_area_new();
        gtk_widget_set_size_request(sidebar_art, 210, 160);
        gtk_widget_set_halign(sidebar_art, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(sidebar_art, GTK_ALIGN_END);
        gtk_widget_set_hexpand(sidebar_art, FALSE);
        gtk_widget_set_vexpand(sidebar_art, FALSE);
        g_signal_connect(
            sidebar_art, "draw", G_CALLBACK(draw_sidebar_art), nullptr);
        gtk_box_pack_end(
            GTK_BOX(sidebar), sidebar_art, FALSE, FALSE, 4);

        pages_ = gtk_stack_new();
        gtk_stack_set_transition_type(
            GTK_STACK(pages_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_set_transition_duration(GTK_STACK(pages_), 110U);
        gtk_paned_pack2(GTK_PANED(paned), pages_, TRUE, TRUE);

        auto* outer_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(
            GTK_SCROLLED_WINDOW(outer_scroll),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_stack_add_named(GTK_STACK(pages_), outer_scroll, "overview");

        auto* base = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        css_class(base, "app-shell");
        gtk_container_set_border_width(GTK_CONTAINER(base), 12);
        gtk_container_add(GTK_CONTAINER(outer_scroll), base);

        auto* hero = gtk_overlay_new();
        css_class(hero, "hero");
        auto* hero_art = gtk_drawing_area_new();
        gtk_widget_set_hexpand(hero_art, TRUE);
        gtk_widget_set_size_request(hero_art, -1, 136);
        g_signal_connect(
            hero_art, "draw", G_CALLBACK(draw_hero_art), nullptr);
        gtk_container_add(GTK_CONTAINER(hero), hero_art);

        auto* hero_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(hero_content), 16);
        gtk_widget_set_valign(hero_content, GTK_ALIGN_CENTER);

        auto* drive_badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        css_class(drive_badge, "hero-drive");
        auto* drive_art = gtk_drawing_area_new();
        gtk_widget_set_size_request(drive_art, 84, 74);
        gtk_widget_set_hexpand(drive_art, FALSE);
        gtk_widget_set_vexpand(drive_art, FALSE);
        g_signal_connect(
            drive_art, "draw", G_CALLBACK(draw_drive_art), nullptr);
        gtk_box_pack_start(
            GTK_BOX(drive_badge), drive_art, FALSE, FALSE, 0);
        gtk_box_pack_start(
            GTK_BOX(hero_content), drive_badge, FALSE, FALSE, 0);

        auto* hero_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        auto* hero_kicker = gtk_label_new("SELECTED VOLUME");
        gtk_label_set_xalign(GTK_LABEL(hero_kicker), 0);
        css_class(hero_kicker, "kicker");
        gtk_box_pack_start(GTK_BOX(hero_text), hero_kicker, FALSE, FALSE, 0);
        volume_title_ = gtk_label_new("Choose a disk");
        gtk_label_set_xalign(GTK_LABEL(volume_title_), 0);
        gtk_label_set_ellipsize(GTK_LABEL(volume_title_), PANGO_ELLIPSIZE_END);
        css_class(volume_title_, "hero-title");
        gtk_box_pack_start(
            GTK_BOX(hero_text), volume_title_, FALSE, FALSE, 0);
        detail_ = gtk_label_new("Choose a volume or open a filesystem image.");
        gtk_label_set_xalign(GTK_LABEL(detail_), 0);
        gtk_label_set_ellipsize(GTK_LABEL(detail_), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(detail_), 78);
        css_class(detail_, "hint");
        gtk_box_pack_start(GTK_BOX(hero_text), detail_, FALSE, FALSE, 0);
        gtk_box_pack_start(
            GTK_BOX(hero_content), hero_text, TRUE, TRUE, 0);

        hero_status_ = gtk_label_new("Ready");
        css_class(hero_status_, "hero-status");
        gtk_box_pack_end(
            GTK_BOX(hero_content), hero_status_, FALSE, FALSE, 0);

        auto* version_badge = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        css_class(version_badge, "version-badge");
        auto* version_label = gtk_label_new(("Version " + std::string(LD_VERSION)).c_str());
        gtk_label_set_xalign(GTK_LABEL(version_label), 1);
        auto* native_label = gtk_label_new("Native C++");
        gtk_label_set_xalign(GTK_LABEL(native_label), 1);
        gtk_box_pack_start(
            GTK_BOX(version_badge), version_label, FALSE, FALSE, 0);
        gtk_box_pack_start(
            GTK_BOX(version_badge), native_label, FALSE, FALSE, 0);
        gtk_box_pack_end(
            GTK_BOX(hero_content), version_badge, FALSE, FALSE, 0);

        gtk_overlay_add_overlay(GTK_OVERLAY(hero), hero_content);
        gtk_box_pack_start(GTK_BOX(base), hero, FALSE, FALSE, 0);

        auto* selector_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        auto* selector_title = gtk_label_new("VOLUME");
        css_class(selector_title, "kicker");
        gtk_label_set_xalign(GTK_LABEL(selector_title), 0);
        gtk_box_pack_start(GTK_BOX(selector_box), selector_title, FALSE, FALSE, 0);
        auto* selector = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(selector_box), selector, FALSE, FALSE, 0);
        auto* selector_panel = section(nullptr, selector_box);
        css_class(selector_panel, "volume-selector-panel");
        gtk_box_pack_start(
            GTK_BOX(base), selector_panel, FALSE, FALSE, 0);
        volumes_widget_ = gtk_combo_box_text_new();
        gtk_widget_set_hexpand(volumes_widget_, TRUE);
        gtk_box_pack_start(GTK_BOX(selector), volumes_widget_, TRUE, TRUE, 0);
        g_signal_connect(volumes_widget_, "changed", G_CALLBACK(selected), this);
        refresh_ = add_button(selector, "Refresh", "refresh");
        image_ = add_button(selector, "Open image", "image");
        unmount_ = add_button(selector, "Unmount", "unmount");
        auto* cards = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(cards), 10);
        gtk_grid_set_column_homogeneous(GTK_GRID(cards), TRUE);
        const char* captions[] = {"FRAGMENTATION", "FREE SPACE", "ALLOCATED", "FILES"};
        for (int i = 0; i < 4; ++i) {
            auto* card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            css_class(card, "card");
            if (i == 0) css_class(card, "summary-fragmented");
            else if (i == 1) css_class(card, "summary-free");
            else if (i == 2) css_class(card, "summary-capacity");
            else css_class(card, "summary-files");
            gtk_container_set_border_width(GTK_CONTAINER(card), 10);

            if (i < 3) {
                gauges_[i] = gtk_drawing_area_new();
                gtk_widget_set_size_request(gauges_[i], 94, 94);
                g_object_set_data(
                    G_OBJECT(gauges_[i]), "gauge-index", GINT_TO_POINTER(i));
                g_signal_connect(
                    gauges_[i], "draw", G_CALLBACK(draw_gauge), this);
                gtk_box_pack_start(
                    GTK_BOX(card), gauges_[i], FALSE, FALSE, 0);
            }

            auto* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
            gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
            auto* caption = gtk_label_new(captions[i]);
            gtk_label_set_xalign(GTK_LABEL(caption), 0);
            css_class(caption, "gauge-title");
            gtk_box_pack_start(GTK_BOX(copy), caption, FALSE, FALSE, 0);
            cards_[i] = gtk_label_new("—");
            css_class(cards_[i], "summary-value");
            gtk_label_set_xalign(GTK_LABEL(cards_[i]), 0);
            gtk_label_set_line_wrap(GTK_LABEL(cards_[i]), TRUE);
            gtk_box_pack_start(GTK_BOX(copy), cards_[i], FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(card), copy, TRUE, TRUE, 0);
            gtk_grid_attach(GTK_GRID(cards), card, i, 0, 1, 1);
        }
        gtk_box_pack_start(GTK_BOX(base), cards, FALSE, FALSE, 0);

        auto* map_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        auto* map_hint = gtk_label_new("DISK MAP   ·   Physical position, left to right then top to bottom");
        css_class(map_hint, "kicker");
        gtk_label_set_xalign(GTK_LABEL(map_hint), 0);
        gtk_box_pack_start(GTK_BOX(map_box), map_hint, FALSE, FALSE, 0);
        map_ = gtk_drawing_area_new();
        gtk_widget_set_size_request(map_, -1, 64);
        gtk_box_pack_start(GTK_BOX(map_box), map_, TRUE, TRUE, 0);
        g_signal_connect(map_, "draw", G_CALLBACK(draw_map), this);
        legend_ = gtk_label_new(nullptr);
        update_legend();
        gtk_label_set_xalign(GTK_LABEL(legend_), 0);
        gtk_box_pack_start(GTK_BOX(map_box), legend_, FALSE, FALSE, 0);
        summary_ = gtk_label_new("Run Analyse to inspect the allocation map.");
        gtk_label_set_xalign(GTK_LABEL(summary_), 0);
        gtk_box_pack_start(GTK_BOX(map_box), summary_, FALSE, FALSE, 0);
        auto* map_panel = section(nullptr, map_box);
        css_class(map_panel, "map-panel");
        gtk_box_pack_start(
            GTK_BOX(base), map_panel, TRUE, TRUE, 0);

        auto* actions = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(actions), 10);
        gtk_grid_set_column_homogeneous(GTK_GRID(actions), TRUE);
        analyse_ = make_action_button(
            "system-search-symbolic", "Analyse", "Scan and visualise",
            "analyse");
        defrag_ = make_action_button(
            "view-grid-symbolic", "Defragment", "Optimise file layout",
            "defrag");
        growth_ = make_action_button(
            "go-up-symbolic", "Growth Defrag", "Keep growth space contiguous",
            "growth-defrag");
        recover_ = make_action_button(
            "edit-undo-symbolic", "Recover", "Resume safe recovery",
            "recover");
        gtk_grid_attach(GTK_GRID(actions), analyse_, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(actions), defrag_, 1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(actions), growth_, 2, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(actions), recover_, 3, 0, 1, 1);
        gtk_box_pack_start(GTK_BOX(base), actions, FALSE, FALSE, 0);

        auto* activity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        auto* activity_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        auto* activity_icon = gtk_image_new_from_icon_name(
            "media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_image_set_pixel_size(GTK_IMAGE(activity_icon), 18);
        gtk_box_pack_start(
            GTK_BOX(activity_row), activity_icon, FALSE, FALSE, 0);

        status_ = gtk_label_new("Ready for analysis");
        gtk_label_set_xalign(GTK_LABEL(status_), 0);
        css_class(status_, "activity-primary");
        gtk_box_pack_start(
            GTK_BOX(activity_row), status_, FALSE, FALSE, 0);

        activity_secondary_ = gtk_label_new(
            "Choose a volume; analysis and safe operations appear here.");
        gtk_label_set_xalign(GTK_LABEL(activity_secondary_), 0);
        gtk_label_set_ellipsize(
            GTK_LABEL(activity_secondary_), PANGO_ELLIPSIZE_END);
        css_class(activity_secondary_, "activity-secondary");
        gtk_box_pack_start(
            GTK_BOX(activity_row), activity_secondary_, TRUE, TRUE, 0);

        progress_ = gtk_progress_bar_new();
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_), TRUE);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_), "Ready");
        gtk_widget_set_size_request(progress_, 220, -1);
        gtk_box_pack_start(
            GTK_BOX(activity_row), progress_, TRUE, TRUE, 8);

        stop_ = add_button(activity_row, "Stop safely", "stop");
        css_class(stop_, "stop-action");
        gtk_box_pack_start(GTK_BOX(activity), activity_row, FALSE, FALSE, 0);

        auto* log_expander = gtk_expander_new("Technical activity log");
        gtk_box_pack_start(
            GTK_BOX(activity), log_expander, FALSE, TRUE, 0);
        auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(
            GTK_SCROLLED_WINDOW(scroll),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scroll, -1, 110);
        gtk_container_add(GTK_CONTAINER(log_expander), scroll);
        log_ = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(log_), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(log_), 9);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(log_), 9);
        gtk_container_add(GTK_CONTAINER(scroll), log_);
        auto* activity_panel = section(nullptr, activity);
        css_class(activity_panel, "activity-panel");
        gtk_box_pack_start(
            GTK_BOX(base), activity_panel, FALSE, FALSE, 0);

        auto* status_strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        css_class(status_strip, "status-strip");
        gtk_container_set_border_width(GTK_CONTAINER(status_strip), 8);
        auto* ready_dot = gtk_label_new("●");
        css_class(ready_dot, "ready-dot");
        gtk_box_pack_start(
            GTK_BOX(status_strip), ready_dot, FALSE, FALSE, 0);
        footer_volume_ = gtk_label_new("No volume selected");
        css_class(footer_volume_, "footer-volume");
        gtk_label_set_xalign(GTK_LABEL(footer_volume_), 0);
        gtk_box_pack_start(
            GTK_BOX(status_strip), footer_volume_, FALSE, FALSE, 0);
        auto* status_separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
        gtk_box_pack_start(
            GTK_BOX(status_strip), status_separator, FALSE, FALSE, 8);
        footer_status_ = gtk_label_new("Ready");
        css_class(footer_status_, "status-text");
        gtk_label_set_xalign(GTK_LABEL(footer_status_), 0);
        gtk_box_pack_start(
            GTK_BOX(status_strip), footer_status_, TRUE, TRUE, 0);
        gtk_box_pack_start(
            GTK_BOX(base), status_strip, FALSE, FALSE, 0);

        gtk_stack_add_named(
            GTK_STACK(pages_),
            build_operation_page(
                "Analyse",
                "Scan the selected filesystem and render its true physical allocation.",
                "system-search-symbolic",
                "Analyse selected volume",
                "analyse",
                true),
            "analyse");
        gtk_stack_add_named(
            GTK_STACK(pages_),
            build_operation_page(
                "Defragment",
                "Compact movable extents while preserving the verified filesystem contract.",
                "view-grid-symbolic",
                "Defragment selected volume",
                "defrag",
                true),
            "defrag");
        gtk_stack_add_named(
            GTK_STACK(pages_),
            build_operation_page(
                "Growth Defrag",
                "Keep at least the qualified growth reserve contiguous after file data.",
                "go-up-symbolic",
                "Growth Defrag selected volume",
                "growth-defrag",
                true),
            "growth");
        gtk_stack_add_named(
            GTK_STACK(pages_),
            build_operation_page(
                "Recover",
                "Resume a journalled operation without weakening target identity checks.",
                "edit-undo-symbolic",
                "Recover selected volume",
                "recover",
                false),
            "recover");
        gtk_stack_add_named(
            GTK_STACK(pages_), build_test_media_page(), "test-media");
        gtk_stack_add_named(
            GTK_STACK(pages_), build_settings_page(), "settings");
        gtk_stack_set_visible_child_name(GTK_STACK(pages_), "overview");

        gtk_widget_show_all(window_);
        refresh();
        // Request authentication once per session, matching the existing desktop behavior.
        auth_timer_ = g_timeout_add(200, [](gpointer data) -> gboolean {
            static_cast<Desktop*>(data)->start_helper();
            static_cast<Desktop*>(data)->auth_timer_ = 0;
            return G_SOURCE_REMOVE;
        }, this);
    }

    ~Desktop() {
        if (auth_timer_) g_source_remove(auth_timer_);
        if (local_stop_timer_) g_source_remove(local_stop_timer_);
        if (selection_analysis_timer_)
            g_source_remove(selection_analysis_timer_);
        if (helper_) {
            // Never close an active helper transport; the helper treats EOF as a Stop.
            if (!busy_) {
                if (helper_ready_) {
                    try { write_helper(Json::Object{{"action", Json("quit")}}); }
                    catch (...) { /* The session has already ended. */ }
                } else g_subprocess_force_exit(helper_);
            }
            g_object_unref(helper_);
        }
        if (helper_input_) g_object_unref(helper_input_);
        if (local_) g_object_unref(local_);
        if (discovery_) {
            g_subprocess_force_exit(discovery_);
            g_object_unref(discovery_);
        }
        if (probe_) {
            g_subprocess_force_exit(probe_);
            g_object_unref(probe_);
        }
    }

private:
    GtkWidget *window_{}, *pages_{}, *volumes_widget_{}, *detail_{}, *volume_title_{};
    GtkWidget *progress_{}, *status_{}, *map_{}, *summary_{}, *log_{};
    GtkWidget *hero_status_{}, *activity_secondary_{}, *footer_volume_{}, *footer_status_{};
    GtkWidget *analyse_{}, *unmount_{}, *defrag_{}, *growth_{}, *recover_{}, *stop_{};
    GtkWidget *refresh_{}, *image_{}, *legend_{}, *theme_combo_{};
    GtkWidget *nav_overview_{}, *nav_analyse_{}, *nav_defrag_{}, *nav_growth_{};
    GtkWidget *nav_recover_{}, *nav_test_media_{}, *nav_settings_{};
    GtkWidget *page_analyse_{}, *page_defrag_{}, *page_growth_{}, *page_recover_{};
    InfiltratrThemeMode theme_mode_ = INFILTRATR_THEME_SYSTEM;
    const InfiltratrThemePalette* palette_ = nullptr;
    const InfiltratrThemePalette* map_palette_ = nullptr;
    GtkWidget* cards_[4]{};
    GtkWidget* gauges_[3]{};
    double gauge_values_[3]{};
    std::vector<GtkWidget*> detail_maps_;
    std::vector<GtkWidget*> page_volume_labels_;
    std::vector<DesktopVolume> volumes_;
    std::vector<Json> cells_;
    std::string mapper_, engine_, helper_path_, pending_program_, purpose_, output_, result_status_;
    std::vector<std::string> pending_args_;
    Json map_data_;
    GSubprocess* helper_ = nullptr;
    GSubprocess* local_ = nullptr;
    GSubprocess* discovery_ = nullptr;
    GSubprocess* probe_ = nullptr;
    GDataInputStream* helper_input_ = nullptr;
    std::string refresh_selected_path_, pending_image_path_;
    bool helper_ready_ = false, busy_ = false, stopping_ = false, closing_ = false;
    bool discovering_ = false, probing_ = false;
    int request_id_ = 0, active_id_ = 0;
    guint auth_timer_ = 0;
    guint local_stop_timer_ = 0;
    guint selection_analysis_timer_ = 0;

    GtkWidget* build_operation_page(
        const char* title,
        const char* subtitle,
        const char* icon_name,
        const char* button_text,
        const char* action_name,
        bool show_map)
    {
        auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(
            GTK_SCROLLED_WINDOW(scroll),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

        auto* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        css_class(page, "operation-page");
        gtk_container_set_border_width(GTK_CONTAINER(page), 24);
        gtk_container_add(GTK_CONTAINER(scroll), page);

        auto* heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        auto* icon = gtk_image_new_from_icon_name(
            icon_name, GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 44);
        gtk_box_pack_start(GTK_BOX(heading), icon, FALSE, FALSE, 0);
        auto* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        auto* title_label = gtk_label_new(title);
        css_class(title_label, "page-title");
        gtk_label_set_xalign(GTK_LABEL(title_label), 0);
        auto* subtitle_label = gtk_label_new(subtitle);
        css_class(subtitle_label, "page-subtitle");
        gtk_label_set_xalign(GTK_LABEL(subtitle_label), 0);
        gtk_label_set_line_wrap(GTK_LABEL(subtitle_label), TRUE);
        gtk_box_pack_start(
            GTK_BOX(copy), title_label, FALSE, FALSE, 0);
        gtk_box_pack_start(
            GTK_BOX(copy), subtitle_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(heading), copy, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);

        auto* volume_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        auto* volume_art = gtk_drawing_area_new();
        gtk_widget_set_size_request(volume_art, 84, 74);
        gtk_widget_set_hexpand(volume_art, FALSE);
        gtk_widget_set_vexpand(volume_art, FALSE);
        g_signal_connect(
            volume_art, "draw", G_CALLBACK(draw_drive_art), nullptr);
        gtk_box_pack_start(
            GTK_BOX(volume_box), volume_art, FALSE, FALSE, 0);
        auto* selected = gtk_label_new("No volume selected");
        css_class(selected, "page-volume");
        gtk_label_set_xalign(GTK_LABEL(selected), 0);
        gtk_label_set_ellipsize(
            GTK_LABEL(selected), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(
            GTK_BOX(volume_box), selected, TRUE, TRUE, 0);
        page_volume_labels_.push_back(selected);
        gtk_box_pack_start(
            GTK_BOX(page), section(nullptr, volume_box), FALSE, FALSE, 0);

        if (show_map) {
            auto* map_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
            auto* label = gtk_label_new("CURRENT PHYSICAL ALLOCATION");
            css_class(label, "kicker");
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_box_pack_start(
                GTK_BOX(map_box), label, FALSE, FALSE, 0);
            auto* detail_map = gtk_drawing_area_new();
            gtk_widget_set_size_request(detail_map, -1, 320);
            g_signal_connect(
                detail_map, "draw", G_CALLBACK(draw_map), this);
            detail_maps_.push_back(detail_map);
            gtk_box_pack_start(
                GTK_BOX(map_box), detail_map, TRUE, TRUE, 0);
            gtk_box_pack_start(
                GTK_BOX(page), section(nullptr, map_box), TRUE, TRUE, 0);
        }

        auto* action_button = make_action_button(
            icon_name, button_text,
            "Run this operation on the selected volume", action_name);
        gtk_box_pack_start(
            GTK_BOX(page), action_button, FALSE, FALSE, 0);
        const std::string action_id(action_name);
        if (action_id == "analyse") page_analyse_ = action_button;
        else if (action_id == "defrag") page_defrag_ = action_button;
        else if (action_id == "growth-defrag") page_growth_ = action_button;
        else if (action_id == "recover") page_recover_ = action_button;
        return scroll;
    }

    GtkWidget* build_test_media_page()
    {
        return build_operation_page(
            "Test Media",
            "Create and diagnose sacrificial filesystem media in the native companion tool.",
            "drive-removable-media-symbolic",
            "Launch Test Media",
            "test-media",
            false);
    }

    GtkWidget* build_settings_page()
    {
        auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(
            GTK_SCROLLED_WINDOW(scroll),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        auto* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        css_class(page, "operation-page");
        gtk_container_set_border_width(GTK_CONTAINER(page), 22);
        gtk_container_add(GTK_CONTAINER(scroll), page);

        auto* title = gtk_label_new("Settings");
        css_class(title, "page-title");
        gtk_label_set_xalign(GTK_LABEL(title), 0);
        gtk_box_pack_start(GTK_BOX(page), title, FALSE, FALSE, 0);

        auto* note = gtk_label_new(
            "Appearance follows the same Common Day/Night contract as the rest of Infiltrator OS.");
        css_class(note, "page-subtitle");
        gtk_label_set_xalign(GTK_LABEL(note), 0);
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_box_pack_start(GTK_BOX(page), note, FALSE, FALSE, 0);

        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
        auto* heading = gtk_label_new("APPEARANCE");
        css_class(heading, "kicker");
        gtk_label_set_xalign(GTK_LABEL(heading), 0);
        gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);

        theme_combo_ = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(theme_combo_),
            theme_label(INFILTRATR_THEME_SYSTEM));
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(theme_combo_),
            theme_label(INFILTRATR_THEME_DAY));
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(theme_combo_),
            theme_label(INFILTRATR_THEME_NIGHT));
        gtk_combo_box_set_active(
            GTK_COMBO_BOX(theme_combo_), theme_index(theme_mode_));
        g_signal_connect(
            theme_combo_, "changed", G_CALLBACK(theme_changed), this);
        gtk_box_pack_start(
            GTK_BOX(box), theme_combo_, FALSE, FALSE, 0);
        gtk_box_pack_start(
            GTK_BOX(page), section(nullptr, box), FALSE, FALSE, 0);

        auto* about_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
        auto* about_heading = gtk_label_new("ABOUT");
        css_class(about_heading, "kicker");
        gtk_label_set_xalign(GTK_LABEL(about_heading), 0);
        gtk_box_pack_start(
            GTK_BOX(about_box), about_heading, FALSE, FALSE, 0);
        auto* about_note = gtk_label_new(
            "Version, licence and project information.");
        css_class(about_note, "page-subtitle");
        gtk_label_set_xalign(GTK_LABEL(about_note), 0);
        gtk_box_pack_start(
            GTK_BOX(about_box), about_note, FALSE, FALSE, 0);
        add_button(about_box, "About Defragmenter", "about");
        gtk_box_pack_start(
            GTK_BOX(page), section(nullptr, about_box), FALSE, FALSE, 0);
        return scroll;
    }

    static int theme_index(InfiltratrThemeMode mode)
    {
        switch (mode) {
            case INFILTRATR_THEME_DAY: return 1;
            case INFILTRATR_THEME_NIGHT: return 2;
            case INFILTRATR_THEME_SYSTEM:
            default: return 0;
        }
    }

    static GtkWidget* add_nav_button(
        GtkWidget* box,
        const char* icon_name,
        const char* title,
        const char* subtitle,
        const char* action,
        bool selected)
    {
        auto* button = gtk_button_new();
        css_class(button, "nav-button");
        const std::string action_id(action);
        if (action_id == "page-analyse") css_class(button, "nav-analyse");
        else if (action_id == "page-defrag") css_class(button, "nav-defrag");
        else if (action_id == "page-growth") css_class(button, "nav-growth");
        else if (action_id == "page-recover") css_class(button, "nav-recover");
        else if (action_id == "page-test-media") css_class(button, "nav-test-media");
        if (selected) css_class(button, "nav-selected");
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
        auto* icon = gtk_image_new_from_icon_name(
            icon_name, GTK_ICON_SIZE_BUTTON);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 28);
        const char* icon_class = "nav-icon-overview";
        if (action_id == "page-analyse") icon_class = "nav-icon-analyse";
        else if (action_id == "page-defrag") icon_class = "nav-icon-defrag";
        else if (action_id == "page-growth") icon_class = "nav-icon-growth";
        else if (action_id == "page-recover") icon_class = "nav-icon-recover";
        else if (action_id == "page-test-media") icon_class = "nav-icon-test-media";
        else if (action_id == "page-settings") icon_class = "nav-icon-settings";
        css_class(icon, icon_class);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
        auto* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        auto* primary = gtk_label_new(title);
        css_class(primary, "nav-title");
        gtk_label_set_xalign(GTK_LABEL(primary), 0);
        auto* secondary = gtk_label_new(subtitle);
        css_class(secondary, "nav-subtitle");
        gtk_label_set_xalign(GTK_LABEL(secondary), 0);
        gtk_box_pack_start(GTK_BOX(copy), primary, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(copy), secondary, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), copy, TRUE, TRUE, 0);
        auto* arrow = gtk_label_new("›");
        css_class(arrow, "action-arrow");
        gtk_box_pack_end(GTK_BOX(row), arrow, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(button), row);
        g_object_set_data_full(
            G_OBJECT(button), "action", g_strdup(action), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
        return button;
    }

    static GtkWidget* make_action_button(
        const char* icon_name,
        const char* title,
        const char* subtitle,
        const char* action)
    {
        auto* button = gtk_button_new();
        css_class(button, "action-card");
        const std::string action_id(action);
        if (action_id == "analyse") css_class(button, "action-analyse");
        else if (action_id == "defrag") css_class(button, "action-defrag");
        else if (action_id == "growth-defrag" || action_id == "test-media")
            css_class(button, "action-growth");
        else if (action_id == "recover") css_class(button, "action-recover");
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
        auto* icon = gtk_image_new_from_icon_name(
            icon_name, GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
        auto* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        auto* primary = gtk_label_new(title);
        css_class(primary, "action-title");
        gtk_label_set_xalign(GTK_LABEL(primary), 0);
        auto* secondary = gtk_label_new(subtitle);
        css_class(secondary, "action-subtitle");
        gtk_label_set_xalign(GTK_LABEL(secondary), 0);
        gtk_label_set_line_wrap(GTK_LABEL(secondary), TRUE);
        gtk_box_pack_start(GTK_BOX(copy), primary, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(copy), secondary, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), copy, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(button), row);
        g_object_set_data_full(
            G_OBJECT(button), "action", g_strdup(action), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(clicked), nullptr);
        return button;
    }

    static GtkWidget* add_button(GtkWidget* box, const char* label, const char* action) {
        auto* widget = gtk_button_new_with_label(label);
        g_object_set_data_full(G_OBJECT(widget), "action", g_strdup(action), g_free);
        gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
        g_signal_connect(widget, "clicked", G_CALLBACK(clicked), nullptr);
        return widget;
    }
    static Desktop* owner(GtkWidget* widget) {
        auto* top = gtk_widget_get_toplevel(widget);
        return static_cast<Desktop*>(g_object_get_data(G_OBJECT(top), "desktop"));
    }
    static void activated(GtkMenuItem* item, gpointer data) {
        auto* self = static_cast<Desktop*>(data);
        if (self) self->action(static_cast<const char*>(
            g_object_get_data(G_OBJECT(item), "action")));
    }
    static gboolean draw_gauge(GtkWidget* widget, cairo_t* cr, gpointer data) {
        auto* self = static_cast<Desktop*>(data);
        const int index = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(widget), "gauge-index"));
        if (index < 0 || index >= 3 || self->palette_ == nullptr)
            return FALSE;

        GtkAllocation allocation;
        gtk_widget_get_allocation(widget, &allocation);
        const double cx = allocation.width / 2.0;
        const double cy = allocation.height / 2.0;
        const double radius = std::max(
            8.0, std::min(allocation.width, allocation.height) / 2.0 - 8.0);
        const double value = std::clamp(
            self->gauge_values_[index], 0.0, 1.0);
        const auto set_rgb = [cr](std::uint32_t rgb) {
            cairo_set_source_rgb(
                cr,
                static_cast<double>((rgb >> 16U) & 0xffU) / 255.0,
                static_cast<double>((rgb >> 8U) & 0xffU) / 255.0,
                static_cast<double>(rgb & 0xffU) / 255.0);
        };

        cairo_set_line_width(cr, 7.0);
        set_rgb(self->palette_->border_rgb);
        cairo_arc(cr, cx, cy, radius, 0.0, 6.283185307179586);
        cairo_stroke(cr);

        const std::uint32_t colour =
            index == 0 ? self->palette_->fault_rgb :
            index == 1 ? self->palette_->neutral_accent_rgb :
                         self->palette_->warning_rgb;
        set_rgb(colour);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_arc(
            cr, cx, cy, radius, -1.5707963267948966,
            -1.5707963267948966 + value * 6.283185307179586);
        cairo_stroke(cr);

        char text[16];
        std::snprintf(
            text, sizeof(text), "%.0f%%", value * 100.0);
        const auto* typography = infiltratr_typography();
        if (typography != nullptr)
            cairo_select_font_face(
                cr, typography->ui_family,
                CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12.0);
        cairo_text_extents_t extents;
        cairo_text_extents(cr, text, &extents);
        set_rgb(self->palette_->heading_rgb);
        cairo_move_to(
            cr,
            cx - (extents.width / 2.0 + extents.x_bearing),
            cy - (extents.height / 2.0 + extents.y_bearing));
        cairo_show_text(cr, text);
        return FALSE;
    }
    static void clicked(GtkButton* button, gpointer) {
        auto* self = owner(GTK_WIDGET(button));
        if (self) self->action(static_cast<const char*>(g_object_get_data(G_OBJECT(button), "action")));
    }
    static void selected(GtkComboBox* combo, gpointer data) {
        auto* self = static_cast<Desktop*>(data);
        self->cells_.clear();
        self->map_data_ = Json();
        self->gauge_values_[0] = 0.0;
        self->gauge_values_[1] = 0.0;
        self->gauge_values_[2] = 0.0;
        self->queue_maps();
        self->queue_gauges();
        self->reset_summary();
        self->update();

        if (self->selection_analysis_timer_ != 0U) {
            g_source_remove(self->selection_analysis_timer_);
            self->selection_analysis_timer_ = 0U;
        }
        if (gtk_combo_box_get_active(combo) >= 0) {
            self->selection_analysis_timer_ = g_idle_add(
                [](gpointer pointer) -> gboolean {
                    auto* desktop = static_cast<Desktop*>(pointer);
                    desktop->selection_analysis_timer_ = 0U;
                    if (!desktop->closing_ && !desktop->busy_ &&
                        !desktop->discovering_ && !desktop->probing_ &&
                        desktop->current() != nullptr)
                        desktop->action("analyse");
                    return G_SOURCE_REMOVE;
                },
                self);
        }
    }
    static void theme_changed(GtkComboBox* combo, gpointer data) {
        auto* self = static_cast<Desktop*>(data);
        const int index = gtk_combo_box_get_active(combo);
        InfiltratrThemeMode mode = INFILTRATR_THEME_SYSTEM;
        if (index == 1) mode = INFILTRATR_THEME_DAY;
        else if (index == 2) mode = INFILTRATR_THEME_NIGHT;
        self->set_theme(mode);
    }

    static gboolean close_requested(GtkWidget*, GdkEvent*, gpointer data) {
        auto* self = static_cast<Desktop*>(data);
        if (self->busy_) {
            self->closing_ = true;
            self->request_stop();
            self->note("Waiting for the active operation to finish its journalled transaction before closing.");
            return TRUE;
        }
        if (self->discovering_ || self->probing_) {
            self->closing_ = true;
            if (self->discovery_)
                g_subprocess_force_exit(self->discovery_);
            if (self->probe_)
                g_subprocess_send_signal(self->probe_, SIGTERM);
            return TRUE;
        }
        gtk_main_quit();
        return FALSE;
    }
    DesktopVolume* current() {
        int index = gtk_combo_box_get_active(GTK_COMBO_BOX(volumes_widget_));
        return index >= 0 && static_cast<size_t>(index) < volumes_.size() ? &volumes_[index] : nullptr;
    }
    void switch_page(const std::string& name) {
        if (pages_ == nullptr) return;
        gtk_stack_set_visible_child_name(GTK_STACK(pages_), name.c_str());
        const struct {
            const char* name;
            GtkWidget* widget;
        } navigation[] = {
            {"overview", nav_overview_},
            {"analyse", nav_analyse_},
            {"defrag", nav_defrag_},
            {"growth", nav_growth_},
            {"recover", nav_recover_},
            {"test-media", nav_test_media_},
            {"settings", nav_settings_},
        };
        for (const auto& item : navigation) {
            if (item.widget == nullptr) continue;
            auto* context = gtk_widget_get_style_context(item.widget);
            gtk_style_context_remove_class(context, "nav-selected");
            if (name == item.name)
                gtk_style_context_add_class(context, "nav-selected");
        }
    }

    void set_theme(InfiltratrThemeMode mode) {
        theme_mode_ = mode;
        save_theme_mode(theme_mode_);
        try {
            palette_ = install_style(theme_mode_);
            if (theme_combo_ != nullptr &&
                gtk_combo_box_get_active(GTK_COMBO_BOX(theme_combo_)) !=
                    theme_index(theme_mode_))
                gtk_combo_box_set_active(
                    GTK_COMBO_BOX(theme_combo_), theme_index(theme_mode_));
            update_legend();
            queue_maps();
            queue_gauges();
        } catch (const std::exception& ex) {
            error("Unable to change appearance", ex.what());
        }
    }

    void queue_maps() {
        if (map_ != nullptr) gtk_widget_queue_draw(map_);
        for (auto* widget : detail_maps_)
            if (widget != nullptr) gtk_widget_queue_draw(widget);
    }

    void queue_gauges() {
        for (auto* widget : gauges_)
            if (widget != nullptr) gtk_widget_queue_draw(widget);
    }

    void reset_summary() {
        for (auto* label : cards_)
            if (label != nullptr)
                gtk_label_set_text(GTK_LABEL(label), "—");
        if (summary_ != nullptr)
            gtk_label_set_text(
                GTK_LABEL(summary_),
                "Run Analyse to inspect the allocation map.");
    }

    std::uint64_t desired_map_cells() const {
        if (map_ == nullptr) return 4096U;
        GtkAllocation allocation;
        gtk_widget_get_allocation(map_, &allocation);
        const std::uint64_t width =
            static_cast<std::uint64_t>(std::max(1, allocation.width));
        const std::uint64_t height =
            static_cast<std::uint64_t>(std::max(1, allocation.height));
        const std::uint64_t pixels = width * height;
        return std::clamp<std::uint64_t>(
            pixels, 256U, 1048576U);
    }

    void note(const std::string& message) {
        GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_));
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_insert(buffer, &end, (message + "\n").c_str(), -1);
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(log_), &end, 0, FALSE, 0, 0);
    }
    void error(const std::string& title, const std::string& message) {
        note(title + ": " + message);
        auto* dialog = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", title.c_str());
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
    bool confirm(const std::string& title, const std::string& message) {
        auto* dialog = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_CANCEL, "%s", title.c_str());
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
        gtk_dialog_add_button(GTK_DIALOG(dialog), "Continue", GTK_RESPONSE_ACCEPT);
        bool accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
        gtk_widget_destroy(dialog);
        return accepted;
    }
    void update_legend() {
        if (legend_ == nullptr) return;
        const std::string markup =
            "<span foreground='#0585FF'>■</span> Used   "
            "<span foreground='#FF253C'>■</span> Fragmented   "
            "<span foreground='#9E2BFA'>■</span> Directory   "
            "<span foreground='#050D1B'>■</span> Free   "
            "<span foreground='#FF9F0A'>■</span> Metadata / reserved";
        gtk_label_set_markup(GTK_LABEL(legend_), markup.c_str());
    }
    void update() {
        auto* v = current();
        std::string page_identity = "No volume selected";
        if (v) {
            gtk_label_set_text(
                GTK_LABEL(volume_title_),
                v->label.empty() ? v->path.c_str() : v->label.c_str());
            if (hero_status_ != nullptr)
                gtk_label_set_text(
                    GTK_LABEL(hero_status_),
                    v->mounted ? "● Mounted" : "● Offline");
            if (footer_volume_ != nullptr)
                gtk_label_set_text(
                    GTK_LABEL(footer_volume_),
                    v->label.empty() ? v->path.c_str() : v->label.c_str());
            std::string details =
                v->path + " · " + v->filesystem + " · " + bytes(v->size)
                + (v->mounted ? " · mounted" : " · unmounted")
                + (v->verified
                    ? " · identity verified"
                    : " · native identity pending");
            gtk_label_set_text(GTK_LABEL(detail_), details.c_str());
            page_identity =
                v->path + " — " + v->filesystem + " — " + bytes(v->size);
        } else {
            gtk_label_set_text(GTK_LABEL(volume_title_), "Choose a disk");
            gtk_label_set_text(
                GTK_LABEL(detail_),
                "Choose a volume or open a filesystem image.");
            if (hero_status_ != nullptr)
                gtk_label_set_text(GTK_LABEL(hero_status_), "Ready");
            if (footer_volume_ != nullptr)
                gtk_label_set_text(
                    GTK_LABEL(footer_volume_), "No volume selected");
        }
        if (footer_status_ != nullptr && status_ != nullptr)
            gtk_label_set_text(
                GTK_LABEL(footer_status_),
                gtk_label_get_text(GTK_LABEL(status_)));
        for (auto* label : page_volume_labels_)
            gtk_label_set_text(
                GTK_LABEL(label), page_identity.c_str());
        bool journal = v && fs::exists(defragger::desktop_journal(*v, getuid()));
        auto state = defragger::desktop_controls(v, busy_, stopping_, journal);
        gtk_widget_set_sensitive(analyse_, state.analyse);
        gtk_widget_set_sensitive(unmount_, state.unmount);
        gtk_widget_set_sensitive(defrag_, state.defrag);
        gtk_widget_set_sensitive(growth_, state.growth_defrag);
        gtk_widget_set_sensitive(recover_, state.recover);
        if (page_analyse_ != nullptr)
            gtk_widget_set_sensitive(page_analyse_, state.analyse);
        if (page_defrag_ != nullptr)
            gtk_widget_set_sensitive(page_defrag_, state.defrag);
        if (page_growth_ != nullptr)
            gtk_widget_set_sensitive(page_growth_, state.growth_defrag);
        if (page_recover_ != nullptr)
            gtk_widget_set_sensitive(page_recover_, state.recover);
        gtk_widget_set_sensitive(stop_, state.stop);
        const bool idle_selection = !busy_ && !discovering_ && !probing_;
        gtk_widget_set_sensitive(refresh_, idle_selection);
        gtk_widget_set_sensitive(image_, idle_selection);
        gtk_widget_set_sensitive(volumes_widget_, idle_selection);
    }
    void maybe_finish_close() {
        if (closing_ && !busy_ && !discovering_ && !probing_)
            gtk_widget_destroy(window_);
    }

    void refresh() {
        if (busy_ || discovering_ || probing_) return;
        refresh_selected_path_ = current() ? current()->path : "";
        const char* argv[] = {
            "lsblk", "--json", "--bytes", "--output",
            "NAME,PATH,TYPE,FSTYPE,FSVER,LABEL,PARTLABEL,UUID,PARTUUID,SERIAL,WWN,START,SIZE,MOUNTPOINTS,RM,RO,MODEL,TRAN",
            nullptr
        };
        GError* failure = nullptr;
        discovery_ = g_subprocess_newv(
            argv,
            static_cast<GSubprocessFlags>(
                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                G_SUBPROCESS_FLAGS_STDERR_PIPE),
            &failure);
        if (!discovery_) {
            std::string detail =
                failure ? failure->message : "unable to start lsblk";
            if (failure) g_error_free(failure);
            error("Unable to discover volumes", detail);
            return;
        }
        discovering_ = true;
        update();
        g_subprocess_communicate_utf8_async(
            discovery_, nullptr, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Desktop*>(data);
                gchar* out = nullptr;
                gchar* err = nullptr;
                GError* callback_error = nullptr;
                const bool received = g_subprocess_communicate_utf8_finish(
                    G_SUBPROCESS(source), result, &out, &err, &callback_error);
                const int code =
                    received &&
                    g_subprocess_get_if_exited(self->discovery_)
                    ? g_subprocess_get_exit_status(self->discovery_)
                    : 127;
                std::string detail =
                    callback_error ? callback_error->message : (err ? err : "");
                try {
                    if (code != 0)
                        throw std::runtime_error(
                            detail.empty() ? "lsblk failed" : detail);
                    std::vector<DesktopVolume> images;
                    for (const auto& volume : self->volumes_)
                        if (volume.image) images.push_back(volume);
                    auto discovered = defragger::desktop_discover(
                        Json::parse(out ? out : ""));
                    discovered.insert(discovered.end(),
                                      images.begin(), images.end());
                    self->volumes_ = std::move(discovered);
                    gtk_combo_box_text_remove_all(
                        GTK_COMBO_BOX_TEXT(self->volumes_widget_));
                    int preferred = 0;
                    for (size_t i = 0; i < self->volumes_.size(); ++i) {
                        const auto& volume = self->volumes_[i];
                        const std::string label =
                            volume.path + " — " + volume.filesystem +
                            " — " + bytes(volume.size);
                        gtk_combo_box_text_append_text(
                            GTK_COMBO_BOX_TEXT(self->volumes_widget_),
                            label.c_str());
                        if (volume.path == self->refresh_selected_path_)
                            preferred = static_cast<int>(i);
                    }
                    if (!self->volumes_.empty())
                        gtk_combo_box_set_active(
                            GTK_COMBO_BOX(self->volumes_widget_), preferred);
                    else
                        gtk_label_set_text(
                            GTK_LABEL(self->detail_),
                            "No supported volumes found. Open a filesystem image to begin.");
                } catch (const std::exception& ex) {
                    if (!self->closing_)
                        self->error("Unable to discover volumes", ex.what());
                }
                if (callback_error) g_error_free(callback_error);
                g_free(out);
                g_free(err);
                g_object_unref(self->discovery_);
                self->discovery_ = nullptr;
                self->discovering_ = false;
                self->update();
                self->maybe_finish_close();
            }, this);
    }

    void open_image() {
        if (busy_ || discovering_ || probing_) return;
        auto* chooser = gtk_file_chooser_dialog_new(
            "Open filesystem image", GTK_WINDOW(window_),
            GTK_FILE_CHOOSER_ACTION_OPEN,
            "Cancel", GTK_RESPONSE_CANCEL,
            "Open", GTK_RESPONSE_ACCEPT, nullptr);
        if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_ACCEPT) {
            gtk_widget_destroy(chooser);
            return;
        }
        char* name =
            gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        gtk_widget_destroy(chooser);
        try {
            fs::path path = fs::canonical(name);
            g_free(name);
            name = nullptr;
            if (!fs::is_regular_file(path))
                throw std::runtime_error(
                    "Select a regular filesystem image.");
            pending_image_path_ = path.string();
        } catch (const std::exception& ex) {
            if (name) g_free(name);
            error("Unable to open image", ex.what());
            return;
        }

        const char* argv[] = {
            mapper_.c_str(), pending_image_path_.c_str(), "--probe", nullptr
        };
        GError* failure = nullptr;
        probe_ = g_subprocess_newv(
            argv,
            static_cast<GSubprocessFlags>(
                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                G_SUBPROCESS_FLAGS_STDERR_PIPE),
            &failure);
        if (!probe_) {
            const std::string detail =
                failure ? failure->message : "unable to start native probe";
            if (failure) g_error_free(failure);
            error("Unable to open image", detail);
            pending_image_path_.clear();
            return;
        }
        probing_ = true;
        update();
        g_subprocess_communicate_utf8_async(
            probe_, nullptr, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Desktop*>(data);
                gchar* out = nullptr;
                gchar* err = nullptr;
                GError* callback_error = nullptr;
                const bool received = g_subprocess_communicate_utf8_finish(
                    G_SUBPROCESS(source), result, &out, &err, &callback_error);
                const int code =
                    received && g_subprocess_get_if_exited(self->probe_)
                    ? g_subprocess_get_exit_status(self->probe_)
                    : 127;
                std::string detail =
                    callback_error ? callback_error->message : (err ? err : "");
                try {
                    if (code != 0)
                        throw std::runtime_error(
                            "Native filesystem probe failed: " + detail);
                    Json probe_data = Json::parse(out ? out : "");
                    DesktopVolume volume;
                    volume.path = self->pending_image_path_;
                    volume.filesystem = field(probe_data, "filesystem");
                    if (!defragger::backend_by_fstype(volume.filesystem))
                        throw std::runtime_error(
                            "Unsupported filesystem image.");
                    std::error_code size_error;
                    volume.size =
                        fs::file_size(volume.path, size_error);
                    if (size_error)
                        throw std::runtime_error(
                            "Filesystem image changed during probing");
                    volume.readonly =
                        access(volume.path.c_str(), W_OK) != 0;
                    volume.image = true;
                    volume.verified = true;
                    self->volumes_.push_back(volume);
                    const std::string label =
                        volume.path + " — " + volume.filesystem +
                        " — " + bytes(volume.size);
                    gtk_combo_box_text_append_text(
                        GTK_COMBO_BOX_TEXT(self->volumes_widget_),
                        label.c_str());
                    gtk_combo_box_set_active(
                        GTK_COMBO_BOX(self->volumes_widget_),
                        static_cast<int>(self->volumes_.size() - 1U));
                    self->cells_.clear();
                    self->map_data_ = Json();
                    self->queue_maps();
                } catch (const std::exception& ex) {
                    if (!self->closing_)
                        self->error("Unable to open image", ex.what());
                }
                if (callback_error) g_error_free(callback_error);
                g_free(out);
                g_free(err);
                g_object_unref(self->probe_);
                self->probe_ = nullptr;
                self->probing_ = false;
                self->pending_image_path_.clear();
                self->update();
                self->maybe_finish_close();
            }, this);
    }

    void action(const std::string& action_name) {
        if (action_name == "minimize") { gtk_window_iconify(GTK_WINDOW(window_)); return; }
        if (action_name == "maximize") {
            if (gtk_window_is_maximized(GTK_WINDOW(window_))) gtk_window_unmaximize(GTK_WINDOW(window_));
            else gtk_window_maximize(GTK_WINDOW(window_));
            return;
        }
        if (action_name == "close") {
            gtk_window_close(GTK_WINDOW(window_));
            return;
        }
        if (action_name.rfind("page-", 0) == 0) {
            switch_page(action_name.substr(5));
            return;
        }
        if (busy_ && action_name != "stop" && action_name != "about") return;
        if (action_name == "refresh") { refresh(); return; }
        if (action_name == "image") { open_image(); return; }
        if (action_name == "stop") { request_stop(); return; }
        if (action_name == "about") {
            const gchar* authors[] = {
                "Shannon Smith — Author and project maintainer", nullptr
            };
            gtk_show_about_dialog(
                GTK_WINDOW(window_),
                "program-name", "Defragmenter",
                "version", LD_VERSION,
                "comments", "Native filesystem analysis and safe layout operations",
                "copyright", "Copyright © 2000–2026 Shannon Smith",
                "website", "https://github.com/Infiltrator-Projects/Defragmenter",
                "website-label", "Website",
                "authors", authors,
                "license-type", GTK_LICENSE_GPL_3_0,
                "wrap-license", TRUE,
                "logo-icon-name", "io.github.linuxdefragger",
                nullptr);
            return;
        }
        if (action_name == "theme") {
            set_theme(infiltratr_theme_mode_next(theme_mode_));
            return;
        }
        if (action_name == "test-media") {
            GError* failure = nullptr;
            try {
                const std::string executable =
                    defragger::resolve_program("test-media");
                const char* argv[] = {executable.c_str(), nullptr};
                auto* process =
                    g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, &failure);
                if (process) {
                    g_object_unref(process);
                } else {
                    const std::string detail =
                        failure ? failure->message : "unable to start Test Media";
                    if (failure) g_error_free(failure);
                    error("Unable to launch Test Media", detail);
                }
            } catch (const std::exception& ex) {
                if (failure) g_error_free(failure);
                error("Unable to launch Test Media", ex.what());
            }
            return;
        }
        auto* v = current();
        if (!v || busy_) return;
        if (action_name == "analyse") {
            v->exact_analysis = false;
            v->defrag_qualified = false;
            v->growth_qualified = false;
            v->defrag_reason.clear();
            v->growth_reason.clear();
            start({
                mapper_, v->path, "--fstype", v->filesystem,
                "--cells", std::to_string(desired_map_cells())},
                "mapper", "analysis");
        } else if (action_name == "unmount") {
            if (confirm("Unmount " + v->path + "?", "The selected volume must be unmounted for raw filesystem operations."))
                start({"udisksctl", "unmount", "-b", v->path}, "udisksctl", "unmount");
        } else {
            try {
                const auto journal = defragger::desktop_journal(*v, getuid());
                auto args = defragger::desktop_mutation(*v, action_name, engine_, journal,
                    4096, fs::exists(journal));
                const auto* backend = defragger::backend_by_fstype(v->filesystem);
                const auto* spec = defragger::operation_for(*backend, action_name);
                std::string message = spec->description + (spec->warning.empty() ? "" : "\n\n" + spec->warning)
                    + "\n\nKeep this volume connected and unmounted. Stop finishes the active journalled transaction.";
                if (!confirm(spec->label + " " + v->path + "?", message)) return;
                start(std::move(args), "operation-engine", action_name);
            } catch (const std::exception& ex) { error("Operation unavailable", ex.what()); }
        }
    }
    void start(std::vector<std::string> args, std::string program, std::string purpose) {
        busy_ = true; stopping_ = false; output_.clear(); result_status_.clear();
        purpose_ = std::move(purpose);
        pending_program_ = std::move(program);
        pending_args_ = std::move(args);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_), 0);
        gtk_progress_bar_set_text(
            GTK_PROGRESS_BAR(progress_), (purpose_ + " in progress").c_str());
        gtk_label_set_text(GTK_LABEL(status_), (purpose_ + " in progress").c_str());
        note("Starting " + purpose_ + " on " + (current() ? current()->path : ""));
        update();
        if (purpose_ == "analysis" && current() && access(current()->path.c_str(), R_OK) == 0) {
            start_local_analysis();
        } else if (helper_ready_) submit();
        else start_helper();
    }
    void start_local_analysis() {
        std::vector<const char*> argv;
        for (const auto& argument : pending_args_) argv.push_back(argument.c_str());
        argv.push_back(nullptr);
        GError* failure = nullptr;
        local_ = g_subprocess_newv(argv.data(),
            static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &failure);
        if (!local_) {
            std::string detail = failure ? failure->message : "Unable to start mapper";
            if (failure) g_error_free(failure);
            completed(127, detail);
            return;
        }
        g_subprocess_communicate_utf8_async(local_, nullptr, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Desktop*>(data);
                gchar* out = nullptr;
                gchar* err = nullptr;
                GError* local_error = nullptr;
                bool received = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err, &local_error);
                self->output_ = out ? out : "";
                std::string detail = local_error ? local_error->message : (err ? err : "");
                int code = received && g_subprocess_get_if_exited(self->local_)
                    ? g_subprocess_get_exit_status(self->local_) : 127;
                if (local_error) g_error_free(local_error);
                g_free(out); g_free(err);
                if (self->local_stop_timer_ != 0U) {
                    g_source_remove(self->local_stop_timer_);
                    self->local_stop_timer_ = 0U;
                }
                g_object_unref(self->local_);
                self->local_ = nullptr;
                self->completed(code, detail);
            }, this);
    }
    void write_helper(const Json& request) {
        if (!helper_) throw std::runtime_error("Administrator helper is unavailable");
        std::string line = request.dump() + "\n";
        GError* failure = nullptr;
        gsize written = 0;
        if (!g_output_stream_write_all(g_subprocess_get_stdin_pipe(helper_),
                line.data(), line.size(), &written, nullptr, &failure)) {
            std::string message = failure ? failure->message : "helper pipe closed";
            if (failure) g_error_free(failure);
            throw std::runtime_error(message);
        }
    }
    void submit() {
        if (!busy_ || !helper_ready_ || active_id_ || local_) return;
        if (stopping_) {
            result_status_ = "stopped";
            completed(0, "Cancelled before the operation started");
            return;
        }
        Json::Array args;
        // The helper enforces the executable allowlist; it receives arguments only.
        for (size_t i = 1; i < pending_args_.size(); ++i) args.emplace_back(pending_args_[i]);
        active_id_ = ++request_id_;
        try {
            write_helper(Json::Object{{"action", Json("run")}, {"id", Json::integer(active_id_)},
                {"program", Json(pending_program_)}, {"argv", Json(std::move(args))}});
        } catch (const std::exception& ex) { completed(127, ex.what()); }
    }
    void start_helper() {
        if (helper_ || helper_ready_) return;
        const char* argv[] = {"pkexec", helper_path_.c_str(), nullptr};
        GError* failure = nullptr;
        helper_ = g_subprocess_newv(argv,
            static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE), &failure);
        if (!helper_) {
            std::string message = failure ? failure->message : "pkexec unavailable";
            if (failure) g_error_free(failure);
            if (busy_) completed(127, message);
            else note("Administrator authentication unavailable: " + message);
            return;
        }
        note("Requesting administrator access for this application session…");
        helper_input_ = g_data_input_stream_new(g_subprocess_get_stdout_pipe(helper_));
        read_helper();
    }
    void read_helper() {
        g_data_input_stream_read_line_async(helper_input_, G_PRIORITY_DEFAULT, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Desktop*>(data);
                GError* failure = nullptr;
                gsize length = 0;
                gchar* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &length, &failure);
                if (!line) {
                    std::string detail = failure ? failure->message : "administrator session ended";
                    if (failure) g_error_free(failure);
                    self->helper_ready_ = false;
                    g_object_unref(self->helper_input_);
                    self->helper_input_ = nullptr;
                    g_object_unref(self->helper_);
                    self->helper_ = nullptr;
                    if (self->busy_ && !self->local_) self->completed(127, detail);
                    else self->note(detail);
                    return;
                }
                try { self->receive(Json::parse(std::string(line, length))); }
                catch (const std::exception& ex) { self->note(std::string("Invalid helper response: ") + ex.what()); }
                g_free(line);
                if (self->helper_ready_ || self->helper_) self->read_helper();
            }, this);
    }
    void receive(const Json& message) {
        if (!message.is_object()) return;
        std::string type = field(message, "type");
        if (type == "ready") {
            helper_ready_ = true;
            note("Administrator session ready.");
            submit();
            return;
        }
        if (type == "error") note(std::string("Helper: ") + field(message, "message"));
        if (type == "stop-result") {
            note(std::string("Stop: ") + field(message, "message"));
            if (const auto* delivered = message.find("delivered"); delivered && !delivered->bool_or()) {
                stopping_ = false; update();
            }
        }
        if (static_cast<int>(number(message, "id")) != active_id_ || !busy_) return;
        if (type == "output") {
            std::string line = field(message, "line");
            if (purpose_ == "analysis") output_ += line + "\n";
            else if (line.rfind("@@", 0) == 0) {
                try { apply_live(line); }
                catch (const std::exception& ex) { note(std::string("Invalid live event: ") + ex.what()); }
            }
            else note(line);
        } else if (type == "progress") {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_),
                std::clamp(message.at("percent").real_value() / 100.0, 0.0, 1.0));
        } else if (type == "finished") {
            completed(static_cast<int>(number(message, "returncode")), "");
        }
    }
    void request_stop() {
        if (!busy_ || stopping_) return;
        stopping_ = true;
        update();
        if (local_) {
            g_subprocess_send_signal(local_, SIGINT);
            note("Stopping read-only analysis and its filesystem worker…");
            if (local_stop_timer_ == 0U) {
                local_stop_timer_ = g_timeout_add_seconds(
                    5U,
                    [](gpointer data) -> gboolean {
                        auto* self = static_cast<Desktop*>(data);
                        self->local_stop_timer_ = 0U;
                        if (self->local_ != nullptr &&
                            self->busy_ && self->stopping_) {
                            self->note(
                                "Analysis did not stop cooperatively; terminating it now.");
                            g_subprocess_force_exit(self->local_);
                        }
                        return G_SOURCE_REMOVE;
                    },
                    this);
            }
            return;
        }
        if (!active_id_) {
            note("Cancelling before the operation starts…");
            if (helper_ready_) submit();
            return;
        }
        try { write_helper(Json::Object{{"action", Json("stop")}, {"id", Json::integer(++request_id_)}}); }
        catch (const std::exception& ex) { stopping_ = false; error("Unable to stop", ex.what()); update(); }
    }
    void completed(int code, const std::string& detail) {
        std::string purpose = purpose_;
        std::string reason = detail;
        bool actually_started = active_id_ != 0 || local_ != nullptr;
        active_id_ = 0;
        busy_ = false; stopping_ = false;
        if (code == 0 && purpose == "analysis") {
            try { present_map(Json::parse(output_)); }
            catch (const std::exception& ex) { code = 127; reason = std::string("Invalid allocation map: ") + ex.what(); }
        }
        if (code == 0 && purpose == "unmount") refresh();
        const bool success = code == 0 && result_status_ != "failed" && result_status_ != "stopped";
        const char* final_status = success
            ? (result_status_ == "not-needed" ? "No changes needed" : "Completed")
            : (result_status_ == "stopped" ? "Stopped safely" : "Operation failed");
        gtk_label_set_text(GTK_LABEL(status_), final_status);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_), final_status);
        if (code != 0 && result_status_ != "stopped") error("Operation failed", reason.empty() ? "Exit status " + std::to_string(code) : reason);
        if (!closing_ && actually_started && (success || result_status_ == "stopped") &&
            (purpose == "defrag" || purpose == "growth-defrag" || purpose == "recover")) {
            update();
            action("analyse");
        }
        update();
        if (closing_ && !busy_) gtk_widget_destroy(window_);
    }
    void present_map(const Json& map) {
        const auto* raw = map.find("cells");
        if (!raw || !raw->is_array() || raw->array().empty() ||
            raw->array().size() > 1048576 ||
            number(map, "cell_count") != raw->array().size() || number(map, "total_units") == 0)
            throw std::runtime_error("Missing or inconsistent allocation cells");
        std::uint64_t end = 0;
        bool first = true;
        for (const auto& cell : raw->array()) {
            if (!cell.is_object() || number(cell, "end") < number(cell, "start") ||
                (!first && number(cell, "start") <= end))
                throw std::runtime_error("Allocation cells overlap or are out of order");
            end = number(cell, "end");
            first = false;
        }
        auto* v = current();
        if (!v) return;
        defragger::desktop_verify_identity(*v, field(map, "filesystem"));
        const std::string accuracy = field(map, "map_accuracy");
        v->exact_analysis =
            !accuracy.empty() && accuracy != "summary" &&
            number(map, "unknown_bytes") == 0U;
        const Json* defrag_qualified = map.find("defrag_qualified");
        const Json* growth_qualified = map.find("growth_qualified");
        v->defrag_qualified =
            v->exact_analysis && defrag_qualified != nullptr &&
            defrag_qualified->bool_or(false);
        v->growth_qualified =
            v->exact_analysis && growth_qualified != nullptr &&
            growth_qualified->bool_or(false);
        v->defrag_reason = field(map, "defrag_reason");
        v->growth_reason = field(map, "growth_reason");
        if (v->exact_analysis && !v->defrag_qualified &&
            !v->defrag_reason.empty())
            note("Defragment unavailable for this exact layout: " +
                 v->defrag_reason);
        if (v->exact_analysis && !v->growth_qualified &&
            !v->growth_reason.empty() &&
            v->growth_reason != v->defrag_reason)
            note("Growth Defrag unavailable for this exact layout: " +
                 v->growth_reason);
        map_data_ = map;
        cells_ = raw->array();
        queue_maps();
        const bool complete_allocation = number(map, "unknown_bytes") == 0U;
        const bool complete_fragmentation =
            v->exact_analysis && map.find("fragmented_files") != nullptr;
        const bool complete_file_count =
            v->exact_analysis && map.find("regular_files") != nullptr;
        std::string summary = "Capacity " + bytes(number(map, "total_bytes")) + "    Free "
            + (complete_allocation ? bytes(number(map, "free_bytes")) : std::string("Not calculated"))
            + "    Used "
            + (complete_allocation ? bytes(number(map, "used_bytes")) : std::string("Not calculated"));
        const std::uint64_t total_bytes = number(map, "total_bytes");
        const std::uint64_t free_bytes = number(map, "free_bytes");
        const std::uint64_t used_bytes = number(map, "used_bytes");
        const std::uint64_t regular_files = number(map, "regular_files");
        const std::uint64_t directories = number(map, "directories");
        const std::uint64_t fragmented_files =
            number(map, "fragmented_files");
        const std::uint64_t fragmented_directories =
            number(map, "fragmented_directories");

        gauge_values_[0] = std::clamp(
            real_number(
                map, "fragmentation_percent",
                regular_files == 0U
                    ? 0.0
                    : 100.0 * static_cast<double>(fragmented_files) /
                        static_cast<double>(regular_files)) /
                100.0,
            0.0, 1.0);
        gauge_values_[1] =
            complete_allocation && total_bytes != 0U
                ? std::clamp(
                    static_cast<double>(free_bytes) /
                        static_cast<double>(total_bytes),
                    0.0, 1.0)
                : 0.0;
        gauge_values_[2] =
            complete_allocation && total_bytes != 0U
                ? std::clamp(
                    static_cast<double>(used_bytes) /
                        static_cast<double>(total_bytes),
                    0.0, 1.0)
                : 0.0;
        queue_gauges();

        const auto percentage = [](double value) {
            char text[24];
            std::snprintf(
                text, sizeof(text), "%.1f%%", value * 100.0);
            return std::string(text);
        };
        gtk_label_set_text(
            GTK_LABEL(cards_[0]),
            complete_fragmentation
                ? (std::to_string(fragmented_files) + " files · " +
                   std::to_string(fragmented_directories) + " dirs").c_str()
                : "Not calculated");
        gtk_label_set_text(
            GTK_LABEL(cards_[1]),
            complete_allocation
                ? (bytes(free_bytes) + " (" +
                   percentage(gauge_values_[1]) + ")").c_str()
                : "Not calculated");
        gtk_label_set_text(
            GTK_LABEL(cards_[2]),
            complete_allocation
                ? (bytes(used_bytes) + " (" +
                   percentage(gauge_values_[2]) + ")").c_str()
                : "Not calculated");
        gtk_label_set_text(
            GTK_LABEL(cards_[3]),
            complete_file_count
                ? (std::to_string(regular_files) + " files · " +
                   std::to_string(directories) + " dirs").c_str()
                : "Not calculated");
        gtk_label_set_text(GTK_LABEL(summary_), summary.c_str());
        note(summary);
    }
    void apply_live(const std::string& line) {
        const auto space = line.find(' ');
        if (space == std::string::npos) return;
        Json payload = Json::parse(std::string_view(line).substr(space + 1));
        if (!payload.is_object()) return;
        const auto kind = line.substr(0, space);
        if (kind == "@@PHASE") {
            gtk_label_set_text(GTK_LABEL(status_), field(payload, "message"));
            note(field(payload, "message"));
        } else if (kind == "@@RESULT") {
            result_status_ = field(payload, "status");
            note(std::string("Result: ") + result_status_ + " " + field(payload, "message"));
        } else if (kind == "@@LIVE_MAP") {
            if (!cells_.empty()) {
                defragger::desktop_live_cells(map_data_, cells_, payload);
                queue_maps();
            }
        } else if (kind == "@@LIVE_RANGE" || kind == "@@LIVE_RANGES") {
            if (!cells_.empty()) {
                defragger::desktop_live_ranges(map_data_, cells_, payload,
                    kind == "@@LIVE_RANGES");
                queue_maps();
            }
        } else if (kind == "@@LIVE_RESET") {
            if (!cells_.empty()) {
                defragger::desktop_live_reset(map_data_, cells_, payload);
                queue_maps();
            }
        }
    }
    static std::uint32_t mix_rgb(
        std::uint32_t first, std::uint32_t second, double ratio)
    {
        ratio = std::clamp(ratio, 0.0, 1.0);
        const auto channel = [ratio](
            std::uint32_t a, std::uint32_t b, unsigned shift) {
            const double av =
                static_cast<double>((a >> shift) & 0xffU);
            const double bv =
                static_cast<double>((b >> shift) & 0xffU);
            return static_cast<std::uint32_t>(
                std::clamp(av * (1.0 - ratio) + bv * ratio, 0.0, 255.0) +
                0.5);
        };
        return (channel(first, second, 16U) << 16U) |
               (channel(first, second, 8U) << 8U) |
               channel(first, second, 0U);
    }

    static std::uint32_t map_cell_rgb(const Json& cell)
    {
        constexpr std::uint32_t free_colour = UINT32_C(0x050D1B);
        constexpr std::uint32_t outside_colour = UINT32_C(0x020408);
        constexpr std::uint32_t used_colour = UINT32_C(0x0585FF);
        constexpr std::uint32_t fragmented_colour = UINT32_C(0xFF253C);
        constexpr std::uint32_t directory_colour = UINT32_C(0x9E2BFA);
        constexpr std::uint32_t unknown_colour = UINT32_C(0x495468);
        constexpr std::uint32_t metadata_colour = UINT32_C(0xFF9F0A);

        const std::uint64_t free = number(cell, "free");
        const std::uint64_t outside = number(cell, "outside");
        const std::uint64_t used = number(cell, "used");
        const std::uint64_t free_like = free + outside;
        const std::uint64_t known = std::max<std::uint64_t>(
            1U, free_like + used);
        std::uint32_t colour = mix_rgb(
            free_colour, outside_colour,
            free_like == 0U
                ? 0.0
                : static_cast<double>(outside) /
                    static_cast<double>(free_like));
        colour = mix_rgb(
            colour, used_colour,
            static_cast<double>(used) /
                static_cast<double>(known));

        const struct Overlay {
            const char* key;
            std::uint32_t colour;
            double minimum;
        } overlays[] = {
            {"directory", directory_colour, 0.58},
            {"fragmented", fragmented_colour, 0.70},
            {"bad", metadata_colour, 0.58},
        };
        for (const auto& overlay : overlays) {
            const std::uint64_t amount = number(cell, overlay.key);
            if (amount == 0U) continue;
            const double ratio = std::max(
                overlay.minimum,
                std::min(
                    1.0,
                    std::sqrt(
                        static_cast<double>(amount) /
                        static_cast<double>(known))));
            colour = mix_rgb(colour, overlay.colour, ratio);
        }

        const std::uint64_t unknown = number(cell, "unknown");
        const std::uint64_t total = free + outside + used + unknown;
        if (unknown != 0U && total != 0U)
            colour = mix_rgb(
                colour, unknown_colour,
                static_cast<double>(unknown) /
                    static_cast<double>(total));
        return colour;
    }

    static gboolean draw_map(GtkWidget* widget, cairo_t* cr, gpointer data) {
        auto* self = static_cast<Desktop*>(data);
        GtkAllocation allocation;
        gtk_widget_get_allocation(widget, &allocation);
        const int width = std::max(1, allocation.width);
        const int height = std::max(1, allocation.height);
        constexpr std::uint32_t background = UINT32_C(0x03050A);

        cairo_set_source_rgb(
            cr, 3.0 / 255.0, 5.0 / 255.0, 10.0 / 255.0);
        cairo_paint(cr);
        if (self->cells_.empty()) return FALSE;

        const std::uint64_t first =
            number(self->cells_.front(), "start");
        const std::uint64_t last =
            number(self->cells_.back(), "end");
        if (last < first) return FALSE;

        const std::size_t pixel_count =
            static_cast<std::size_t>(width) *
            static_cast<std::size_t>(height);
        std::vector<std::uint32_t> pixels(
            pixel_count, UINT32_C(0xFF000000) | background);
        const long double span =
            static_cast<long double>(last - first) + 1.0L;
        std::size_t cell_index = 0U;
        for (std::size_t pixel = 0U;
             pixel < pixel_count; ++pixel) {
            const std::uint64_t unit =
                first + static_cast<std::uint64_t>(
                    static_cast<long double>(pixel) * span /
                    static_cast<long double>(pixel_count));
            while (cell_index < self->cells_.size() &&
                   number(self->cells_[cell_index], "end") < unit)
                ++cell_index;
            if (cell_index >= self->cells_.size()) break;
            const Json& cell = self->cells_[cell_index];
            if (number(cell, "start") > unit) continue;
            pixels[pixel] =
                UINT32_C(0xFF000000) | map_cell_rgb(cell);
        }

        cairo_surface_t* surface =
            cairo_image_surface_create_for_data(
                reinterpret_cast<unsigned char*>(pixels.data()),
                CAIRO_FORMAT_ARGB32,
                width, height,
                width * static_cast<int>(sizeof(std::uint32_t)));
        if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
            cairo_set_source_surface(cr, surface, 0.0, 0.0);
            cairo_pattern_set_filter(
                cairo_get_source(cr), CAIRO_FILTER_NEAREST);
            cairo_paint(cr);
        }
        cairo_surface_destroy(surface);
        return FALSE;
    }
};
} // namespace

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    try {
        Desktop desktop;
        // GTK widget callbacks locate their owner through the top-level window.
        gtk_main();
        return 0;
    } catch (const std::exception& ex) {
        g_printerr("Unable to start Defragmenter: %s\n", ex.what());
        return 1;
    }
}
