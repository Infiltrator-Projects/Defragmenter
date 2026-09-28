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
    const auto* typography = infiltratr_typography();
    const auto* metrics = infiltratr_design_metrics();
    if (palette == nullptr || typography == nullptr || metrics == nullptr)
        throw std::runtime_error("Common design contract is unavailable");

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
    const std::string titlebar = rgb_hex(palette->titlebar_rgb);
    const std::string accent = rgb_hex(palette->neutral_accent_rgb);
    const std::string accent_hover = rgb_hex(palette->accent_hover_rgb);
    const std::string button_bg = rgb_hex(palette->button_background_rgb);
    const std::string button_fg = rgb_hex(palette->button_foreground_rgb);
    const std::string fault = rgb_hex(palette->fault_rgb);
    const std::string operation = rgb_hex(palette->operation_rgb);

    std::string css;
    css.reserve(4096U);
    css += "window, .app-shell { background: " + background +
           "; color: " + text + "; font-family: '" +
           typography->ui_family + "'; }\n";
    css += "headerbar { background: " + titlebar + "; color: " + heading +
           "; border-bottom: 1px solid " + rgb_hex(palette->status_border_rgb) +
           "; min-height: 44px; }\n";
    css += "headerbar button { background: transparent; border: 0; color: " +
           heading + "; box-shadow: none; }\n";
    css += "headerbar button:hover { background: " +
           rgb_hex(palette->surface_hover_rgb) + "; }\n";
    css += ".brand-title { font-family: '" + std::string(typography->brand_family) +
           "'; font-size: 20px; font-weight: 600; color: " + heading + "; }\n";
    css += ".brand-subtitle { font-size: 10px; letter-spacing: 2px; color: " +
           rgb_hex(palette->summary_rgb) + "; }\n";
    css += ".sidebar { background: " + surface +
           "; border-right: 1px solid " + border + "; }\n";
    css += ".sidebar button { background: transparent; color: " + detail +
           "; border: 0; }\n";
    css += ".sidebar button:hover { background: " +
           rgb_hex(palette->card_hover_rgb) + "; color: " + heading + "; }\n";
    css += ".card, .panel { background: " + card + "; border: 1px solid " +
           border + "; border-radius: " + std::to_string(metrics->card_radius) +
           "px; }\n";
    css += ".hero { border: 1px solid " + border + "; border-radius: " +
           std::to_string(metrics->panel_radius) + "px; }\n";
    css += ".hero-title { font-family: '" + std::string(typography->brand_family) +
           "'; font-size: 30px; font-weight: 600; color: " + heading + "; }\n";
    css += ".hint { color: " + detail + "; }\n";
    css += ".kicker { color: " + kicker +
           "; font-size: 11px; letter-spacing: 2px; }\n";
    css += ".summary-value { color: " + heading + "; font-size: 19px; }\n";
    css += "button { border-radius: " +
           std::to_string(metrics->control_radius) +
           "px; padding: 7px 12px; background: " + card + "; color: " + text +
           "; border: 1px solid " + accent + "; }\n";
    css += "button:hover { background: " +
           rgb_hex(palette->card_hover_rgb) + "; border-color: " +
           accent_hover + "; }\n";
    css += ".primary-action { background: " + button_bg + "; color: " +
           button_fg + "; font-weight: bold; }\n";
    css += ".stop-action { background: " + operation + "; color: " + text +
           "; border-color: " + fault + "; }\n";
    css += "textview, textview text { background: " + input + "; color: " +
           text + "; }\n";
    css += "progressbar trough { background: " + panel +
           "; border-radius: " + std::to_string(metrics->small_radius) +
           "px; min-height: 8px; }\n";
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
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        g_object_set_data(G_OBJECT(window_), "desktop", this);
        gtk_window_set_title(GTK_WINDOW(window_), "Defragmenter");
        gtk_window_set_default_size(GTK_WINDOW(window_), 1380, 840);
        gtk_window_set_icon_name(GTK_WINDOW(window_), "io.github.linuxdefragger");
        g_signal_connect(window_, "delete-event", G_CALLBACK(close_requested), this);
        g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {
            gtk_main_quit();
        }), nullptr);
        auto* header = gtk_header_bar_new();
        gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), FALSE);
        gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header), gtk_label_new(""));
        auto* brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        auto* icon = gtk_image_new_from_icon_name("io.github.linuxdefragger", GTK_ICON_SIZE_LARGE_TOOLBAR);
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
        for (const auto* name : {"minimize", "maximize", "close"}) {
            auto* button = gtk_button_new_from_icon_name((std::string("window-") + name + "-symbolic").c_str(), GTK_ICON_SIZE_BUTTON);
            g_object_set_data_full(G_OBJECT(button), "action", g_strdup(name), g_free);
            g_signal_connect(button, "clicked", G_CALLBACK(clicked), nullptr);
            gtk_header_bar_pack_end(GTK_HEADER_BAR(header), button);
        }
        gtk_window_set_titlebar(GTK_WINDOW(window_), header);

        auto* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_container_add(GTK_CONTAINER(window_), paned);
        auto* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        css_class(sidebar, "sidebar");
        gtk_container_set_border_width(GTK_CONTAINER(sidebar), 14);
        gtk_widget_set_size_request(sidebar, 220, -1);
        gtk_paned_pack1(GTK_PANED(paned), sidebar, FALSE, FALSE);
        auto* navigation = gtk_label_new("WORKSPACE");
        css_class(navigation, "kicker");
        gtk_widget_set_halign(navigation, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(sidebar), navigation, FALSE, FALSE, 12);
        auto* overview = gtk_label_new("Overview  ·  Allocation map");
        gtk_label_set_xalign(GTK_LABEL(overview), 0);
        gtk_box_pack_start(GTK_BOX(sidebar), overview, FALSE, FALSE, 0);
        add_button(sidebar, "Test Media", "test-media");
        theme_ = add_button(sidebar, theme_label(theme_mode_), "theme");
        add_button(sidebar, "About", "about");
        auto* sidebar_art = gtk_image_new_from_file(artwork("sidebar-workbench.jpg").string().c_str());
        gtk_box_pack_end(GTK_BOX(sidebar), sidebar_art, FALSE, FALSE, 0);

        auto* outer_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_paned_pack2(GTK_PANED(paned), outer_scroll, TRUE, FALSE);
        auto* base = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        css_class(base, "app-shell");
        gtk_container_set_border_width(GTK_CONTAINER(base), 16);
        gtk_container_add(GTK_CONTAINER(outer_scroll), base);

        auto* hero = gtk_overlay_new();
        css_class(hero, "hero");
        auto* hero_image = gtk_image_new_from_file(artwork("hero-landscape.jpg").string().c_str());
        gtk_widget_set_size_request(hero_image, -1, 150);
        gtk_container_add(GTK_CONTAINER(hero), hero_image);
        auto* hero_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(hero_text), 18);
        gtk_widget_set_halign(hero_text, GTK_ALIGN_START);
        gtk_widget_set_valign(hero_text, GTK_ALIGN_CENTER);
        auto* hero_kicker = gtk_label_new("SELECTED VOLUME");
        gtk_label_set_xalign(GTK_LABEL(hero_kicker), 0);
        css_class(hero_kicker, "kicker");
        gtk_box_pack_start(GTK_BOX(hero_text), hero_kicker, FALSE, FALSE, 0);
        volume_title_ = gtk_label_new("Choose a disk");
        gtk_label_set_xalign(GTK_LABEL(volume_title_), 0);
        css_class(volume_title_, "hero-title");
        gtk_box_pack_start(GTK_BOX(hero_text), volume_title_, FALSE, FALSE, 0);
        detail_ = gtk_label_new("Choose a volume or open a filesystem image.");
        gtk_label_set_xalign(GTK_LABEL(detail_), 0);
        css_class(detail_, "hint");
        gtk_box_pack_start(GTK_BOX(hero_text), detail_, FALSE, FALSE, 0);
        gtk_overlay_add_overlay(GTK_OVERLAY(hero), hero_text);
        gtk_box_pack_start(GTK_BOX(base), hero, FALSE, FALSE, 0);

        auto* selector_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        auto* selector_title = gtk_label_new("VOLUME");
        css_class(selector_title, "kicker");
        gtk_label_set_xalign(GTK_LABEL(selector_title), 0);
        gtk_box_pack_start(GTK_BOX(selector_box), selector_title, FALSE, FALSE, 0);
        auto* selector = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(selector_box), selector, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(base), section(nullptr, selector_box), FALSE, FALSE, 0);
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
            auto* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            css_class(card, "card");
            gtk_container_set_border_width(GTK_CONTAINER(card), 12);
            auto* caption = gtk_label_new(captions[i]);
            gtk_label_set_xalign(GTK_LABEL(caption), 0);
            css_class(caption, "kicker");
            gtk_box_pack_start(GTK_BOX(card), caption, FALSE, FALSE, 0);
            cards_[i] = gtk_label_new("—");
            css_class(cards_[i], "summary-value");
            gtk_label_set_xalign(GTK_LABEL(cards_[i]), 0);
            gtk_box_pack_start(GTK_BOX(card), cards_[i], FALSE, FALSE, 0);
            gtk_grid_attach(GTK_GRID(cards), card, i, 0, 1, 1);
        }
        gtk_box_pack_start(GTK_BOX(base), cards, FALSE, FALSE, 0);

        auto* map_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        auto* map_hint = gtk_label_new("DISK MAP   ·   Physical position, left to right then top to bottom");
        css_class(map_hint, "kicker");
        gtk_label_set_xalign(GTK_LABEL(map_hint), 0);
        gtk_box_pack_start(GTK_BOX(map_box), map_hint, FALSE, FALSE, 0);
        map_ = gtk_drawing_area_new();
        gtk_widget_set_size_request(map_, -1, 200);
        gtk_box_pack_start(GTK_BOX(map_box), map_, TRUE, TRUE, 0);
        g_signal_connect(map_, "draw", G_CALLBACK(draw_map), this);
        legend_ = gtk_label_new(nullptr);
        update_legend();
        gtk_label_set_xalign(GTK_LABEL(legend_), 0);
        gtk_box_pack_start(GTK_BOX(map_box), legend_, FALSE, FALSE, 0);
        summary_ = gtk_label_new("Run Analyse to inspect the allocation map.");
        gtk_label_set_xalign(GTK_LABEL(summary_), 0);
        gtk_box_pack_start(GTK_BOX(map_box), summary_, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(base), section(nullptr, map_box), TRUE, TRUE, 0);

        auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(base), actions, FALSE, FALSE, 0);
        analyse_ = add_button(actions, "Analyse", "analyse");
        css_class(analyse_, "primary-action");
        defrag_ = add_button(actions, "Defragment", "defrag");
        growth_ = add_button(actions, "Growth Defrag", "growth-defrag");
        recover_ = add_button(actions, "Recover", "recover");

        progress_ = gtk_progress_bar_new();
        gtk_box_pack_start(GTK_BOX(base), progress_, FALSE, FALSE, 0);
        stop_ = add_button(actions, "Stop safely", "stop");
        css_class(stop_, "stop-action");
        status_ = gtk_label_new("Ready");
        gtk_label_set_xalign(GTK_LABEL(status_), 0);
        gtk_box_pack_start(GTK_BOX(base), status_, FALSE, FALSE, 0);
        auto* log_expander = gtk_expander_new("Technical activity log");
        gtk_box_pack_start(GTK_BOX(base), log_expander, FALSE, FALSE, 0);
        auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_widget_set_size_request(scroll, -1, 110);
        gtk_container_add(GTK_CONTAINER(log_expander), scroll);
        log_ = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(log_), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_), GTK_WRAP_WORD_CHAR);
        gtk_container_add(GTK_CONTAINER(scroll), log_);
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
    GtkWidget *window_{}, *volumes_widget_{}, *detail_{}, *volume_title_{}, *progress_{}, *status_{};
    GtkWidget *map_{}, *summary_{}, *log_{}, *analyse_{}, *unmount_{};
    GtkWidget *defrag_{}, *growth_{}, *recover_{}, *stop_{};
    GtkWidget *refresh_{}, *image_{}, *theme_{}, *legend_{};
    InfiltratrThemeMode theme_mode_ = INFILTRATR_THEME_SYSTEM;
    const InfiltratrThemePalette* palette_ = nullptr;
    GtkWidget* cards_[4]{};
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
    static void clicked(GtkButton* button, gpointer) {
        auto* self = owner(GTK_WIDGET(button));
        if (self) self->action(static_cast<const char*>(g_object_get_data(G_OBJECT(button), "action")));
    }
    static void selected(GtkComboBox* combo, gpointer data) {
        if (gtk_combo_box_get_active(combo) >= 0) {
            auto* self = static_cast<Desktop*>(data);
            self->cells_.clear();
            self->map_data_ = Json();
            gtk_widget_queue_draw(self->map_);
            self->update();
        }
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
        if (legend_ == nullptr || palette_ == nullptr) return;
        const std::string markup =
            "<span foreground='" + rgb_hex(palette_->neutral_accent_rgb) +
            "'>■</span> Used   <span foreground='" +
            rgb_hex(palette_->fault_rgb) +
            "'>■</span> Fragmented   <span foreground='" +
            rgb_hex(palette_->operation_rgb) +
            "'>■</span> Directory   <span foreground='" +
            rgb_hex(palette_->background_rgb) +
            "'>■</span> Free   <span foreground='" +
            rgb_hex(palette_->warning_rgb) +
            "'>■</span> Metadata / reserved";
        gtk_label_set_markup(GTK_LABEL(legend_), markup.c_str());
    }
    void update() {
        auto* v = current();
        if (v) {
            gtk_label_set_text(GTK_LABEL(volume_title_), v->label.empty() ? v->path.c_str() : v->label.c_str());
            std::string details = v->path + " · " + v->filesystem + " · " + bytes(v->size)
                + (v->mounted ? " · mounted" : " · unmounted")
                + (v->verified ? " · identity verified" : " · native identity pending");
            gtk_label_set_text(GTK_LABEL(detail_), details.c_str());
        } else {
            gtk_label_set_text(GTK_LABEL(volume_title_), "Choose a disk");
        }
        bool journal = v && fs::exists(defragger::desktop_journal(*v, getuid()));
        auto state = defragger::desktop_controls(v, busy_, stopping_, journal);
        gtk_widget_set_sensitive(analyse_, state.analyse);
        gtk_widget_set_sensitive(unmount_, state.unmount);
        gtk_widget_set_sensitive(defrag_, state.defrag);
        gtk_widget_set_sensitive(growth_, state.growth_defrag);
        gtk_widget_set_sensitive(recover_, state.recover);
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
                    gtk_widget_queue_draw(self->map_);
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
        if (action_name == "close") { gtk_window_close(GTK_WINDOW(window_)); return; }
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
            theme_mode_ = infiltratr_theme_mode_next(theme_mode_);
            save_theme_mode(theme_mode_);
            try {
                palette_ = install_style(theme_mode_);
                gtk_button_set_label(GTK_BUTTON(theme_),
                                     theme_label(theme_mode_));
                update_legend();
                gtk_widget_queue_draw(map_);
            } catch (const std::exception& ex) {
                error("Unable to change appearance", ex.what());
            }
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
            start({mapper_, v->path, "--fstype", v->filesystem, "--cells", "4096"}, "mapper", "analysis");
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
        gtk_label_set_text(GTK_LABEL(status_), success ?
            (result_status_ == "not-needed" ? "No changes needed" : "Completed") :
            (result_status_ == "stopped" ? "Stopped safely" : "Operation failed"));
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
        gtk_widget_queue_draw(map_);
        const bool complete_allocation = number(map, "unknown_bytes") == 0U;
        const bool complete_fragmentation =
            v->exact_analysis && map.find("fragmented_files") != nullptr;
        const bool complete_file_count =
            v->exact_analysis && map.find("regular_files") != nullptr;
        std::string summary = "Capacity " + bytes(number(map, "total_bytes")) + "    Free "
            + (complete_allocation ? bytes(number(map, "free_bytes")) : std::string("Not calculated"))
            + "    Used "
            + (complete_allocation ? bytes(number(map, "used_bytes")) : std::string("Not calculated"));
        gtk_label_set_text(GTK_LABEL(cards_[0]), complete_fragmentation ?
            (std::to_string(number(map, "fragmented_files")) + " files").c_str() : "Not calculated");
        gtk_label_set_text(GTK_LABEL(cards_[1]), complete_allocation ?
            bytes(number(map, "free_bytes")).c_str() : "Not calculated");
        gtk_label_set_text(GTK_LABEL(cards_[2]), complete_allocation ?
            bytes(number(map, "used_bytes")).c_str() : "Not calculated");
        gtk_label_set_text(GTK_LABEL(cards_[3]), complete_file_count ?
            (std::to_string(number(map, "regular_files")) + " files").c_str() : "Not calculated");
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
                gtk_widget_queue_draw(map_);
            }
        } else if (kind == "@@LIVE_RANGE" || kind == "@@LIVE_RANGES") {
            if (!cells_.empty()) {
                defragger::desktop_live_ranges(map_data_, cells_, payload,
                    kind == "@@LIVE_RANGES");
                gtk_widget_queue_draw(map_);
            }
        } else if (kind == "@@LIVE_RESET") {
            if (!cells_.empty()) {
                defragger::desktop_live_reset(map_data_, cells_, payload);
                gtk_widget_queue_draw(map_);
            }
        }
    }
    static gboolean draw_map(GtkWidget* widget, cairo_t* cr, gpointer data) {
        auto* self = static_cast<Desktop*>(data);
        GtkAllocation allocation;
        gtk_widget_get_allocation(widget, &allocation);
        const auto* palette = self->palette_;
        const auto cairo_rgb = [cr](std::uint32_t rgb) {
            cairo_set_source_rgb(
                cr,
                static_cast<double>((rgb >> 16U) & 0xffU) / 255.0,
                static_cast<double>((rgb >> 8U) & 0xffU) / 255.0,
                static_cast<double>(rgb & 0xffU) / 255.0);
        };
        cairo_rgb(palette ? palette->background_rgb : 0U);
        cairo_paint(cr);
        if (self->cells_.empty() || palette == nullptr) return FALSE;
        int columns = std::max(1, allocation.width / 8);
        int rows = std::max(1, allocation.height / 8);
        const std::uint64_t first = number(self->cells_.front(), "start");
        const std::uint64_t last = number(self->cells_.back(), "end");
        if (last < first) return FALSE;
        for (int row = 0; row < rows; ++row) for (int column = 0; column < columns; ++column) {
            const auto index = static_cast<std::uint64_t>(row) * columns + column;
            const auto unit = first + static_cast<std::uint64_t>(
                static_cast<long double>(index) * (static_cast<long double>(last) - first + 1.0L) /
                (static_cast<long double>(rows) * columns));
            const auto found = std::lower_bound(self->cells_.begin(), self->cells_.end(), unit,
                [](const Json& cell, std::uint64_t needle) { return number(cell, "end") < needle; });
            if (found == self->cells_.end() || number(*found, "start") > unit) continue;
            const auto& cell = *found;
            if (number(cell, "bad")) cairo_rgb(palette->warning_rgb);
            else if (number(cell, "fragmented")) cairo_rgb(palette->fault_rgb);
            else if (number(cell, "directory")) cairo_rgb(palette->operation_rgb);
            else if (number(cell, "used")) cairo_rgb(palette->neutral_accent_rgb);
            else if (number(cell, "free")) cairo_rgb(palette->background_rgb);
            else if (number(cell, "outside")) cairo_rgb(palette->panel_rgb);
            else cairo_rgb(palette->muted_rgb);
            cairo_rectangle(cr, column * 8, row * 8, 7, 7);
            cairo_fill(cr);
        }
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
