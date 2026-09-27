// SPDX-License-Identifier: GPL-3.0-or-later
// Native GTK desktop client. Filesystem parsing and writes stay in the native engines.
#include "desktop_policy.hpp"
#include "desktop_live_map.hpp"
#include "desktop_map.hpp"
#include "process.hpp"
#include "version.h"

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
void install_style(bool dark) {
    static GtkCssProvider* provider = gtk_css_provider_new();
    constexpr const char* css = R"CSS(
window, .app-shell { background: #050608; color: #E8ECEF; font-family: 'MB Corpo S Title WEB'; }
headerbar { background: #202125; color: #EEF1F3; border-bottom: 1px solid #353A40; min-height: 44px; }
headerbar button { background: transparent; border: 0; color: #EEF1F3; box-shadow: none; }
headerbar button:hover { background: #353A40; }
.brand-title { font-family: 'MB Corpo A Title Cond WEB'; font-size: 20px; font-weight: 600; }
.brand-subtitle { font-size: 10px; letter-spacing: 2px; color: #AEB6BD; }
.sidebar { background: #101318; border-right: 1px solid #353A40; }
.sidebar button { background: transparent; color: #AEB6BD; border: 0; text-align: left; }
.sidebar button:hover { background: #22272D; color: #EEF1F3; }
.card, .panel { background: #171B20; border: 1px solid #353A40; border-radius: 12px; }
.hero { border: 1px solid #353A40; border-radius: 18px; }
.hero-title { font-family: 'MB Corpo A Title Cond WEB'; font-size: 30px; font-weight: 600; color: #FFFFFF; }
.hint { color: #AEB6BD; }
.kicker { color: #00ADEF; font-size: 11px; letter-spacing: 2px; }
.summary-value { color: #E8ECEF; font-size: 19px; }
button { border-radius: 10px; padding: 7px 12px; }
.primary-action { background: #00ADEF; color: #031018; font-weight: bold; }
.stop-action { background: #52282C; color: #FFD9DC; }
textview, textview text { background: #0D1014; color: #D7DDE2; }
progressbar trough { background: #20252B; border-radius: 8px; min-height: 8px; }
progressbar progress { background: #00ADEF; border-radius: 8px; }
)CSS";
    constexpr const char* day = R"CSS(
window, .app-shell { background: #FFFFFF; color: #20252B; }
headerbar { background: #ECEFF2; color: #111418; border-color: #C7CDD3; }
headerbar button { color: #111418; }
headerbar button:hover { background: #DDE2E7; }
.sidebar { background: #F8F9FA; border-color: #C7CDD3; }
.sidebar button { color: #20252B; }
.sidebar button:hover { background: #ECEFF2; color: #111418; }
.card, .panel { background: #F8F9FA; border-color: #C7CDD3; }
.hint { color: #59636C; }
.summary-value { color: #111418; }
textview, textview text { background: #FFFFFF; color: #20252B; }
)CSS";
    const std::string rules = std::string(css) + (dark ? "" : day);
    gtk_css_provider_load_from_data(provider, rules.c_str(), -1, nullptr);
    static bool attached = false;
    if (!attached) {
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        attached = true;
    }
}

class Desktop {
public:
    Desktop() {
        mapper_ = defragger::resolve_program("mapper");
        engine_ = defragger::resolve_program("operation-engine");
        helper_path_ = defragger::resolve_program("helper");
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        g_object_set_data(G_OBJECT(window_), "desktop", this);
        gtk_window_set_title(GTK_WINDOW(window_), "Defragmenter");
        gtk_window_set_default_size(GTK_WINDOW(window_), 1380, 840);
        gtk_window_set_icon_name(GTK_WINDOW(window_), "io.github.linuxdefragger");
        g_signal_connect(window_, "delete-event", G_CALLBACK(close_requested), this);
        g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {
            gtk_main_quit();
        }), nullptr);
        load_theme();
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
        add_button(sidebar, "About", "about");
        theme_button_ = add_button(sidebar, "Theme: Follow system", "theme");
        apply_theme();
        if (auto* settings = gtk_settings_get_default()) {
            theme_settings_ = settings;
            theme_signal_ = g_signal_connect(settings, "notify::gtk-theme-name",
                G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                    auto* self = static_cast<Desktop*>(data);
                    if (self->theme_mode_ == "system") self->apply_theme();
                }), this);
            dark_signal_ = g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme",
                G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                    auto* self = static_cast<Desktop*>(data);
                    if (self->theme_mode_ == "system") self->apply_theme();
                }), this);
        }
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
        auto* hero_image = gtk_drawing_area_new();
        gtk_widget_set_size_request(hero_image, -1, 150);
        auto* hero_pixbuf = gdk_pixbuf_new_from_file(artwork("hero-landscape.jpg").string().c_str(), nullptr);
        if (hero_pixbuf) g_object_set_data_full(G_OBJECT(hero_image), "artwork", hero_pixbuf,
            reinterpret_cast<GDestroyNotify>(g_object_unref));
        g_signal_connect(hero_image, "draw", G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer) -> gboolean {
            GtkAllocation allocation;
            gtk_widget_get_allocation(widget, &allocation);
            cairo_set_source_rgb(cr, 0.03, 0.07, 0.11);
            cairo_paint(cr);
            auto* pixbuf = static_cast<GdkPixbuf*>(g_object_get_data(G_OBJECT(widget), "artwork"));
            if (!pixbuf) return FALSE;
            const double scale = std::max(static_cast<double>(allocation.width) / gdk_pixbuf_get_width(pixbuf),
                static_cast<double>(allocation.height) / gdk_pixbuf_get_height(pixbuf));
            cairo_save(cr);
            cairo_translate(cr, (allocation.width - gdk_pixbuf_get_width(pixbuf) * scale) / 2.0,
                (allocation.height - gdk_pixbuf_get_height(pixbuf) * scale) / 2.0);
            cairo_scale(cr, scale, scale);
            gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_pattern_t* shade = cairo_pattern_create_linear(0, 0, allocation.width, 0);
            cairo_pattern_add_color_stop_rgba(shade, 0.0, 0.01, 0.02, 0.04, 0.82);
            cairo_pattern_add_color_stop_rgba(shade, 0.8, 0.01, 0.02, 0.04, 0.0);
            cairo_set_source(cr, shade);
            cairo_paint(cr);
            cairo_pattern_destroy(shade);
            return FALSE;
        }), nullptr);
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
        gtk_widget_set_has_tooltip(map_, TRUE);
        g_signal_connect(map_, "query-tooltip", G_CALLBACK(+[](GtkWidget* widget, gint x, gint y,
            gboolean keyboard, GtkTooltip* tooltip, gpointer data) -> gboolean {
            auto* self = static_cast<Desktop*>(data);
            if (keyboard || self->cells_.empty() || x < 0 || y < 0) return FALSE;
            GtkAllocation area;
            gtk_widget_get_allocation(widget, &area);
            const int columns = std::max(1, area.width / 8);
            const int rows = std::max(1, area.height / 8);
            const int col = std::min(columns - 1, x / 8);
            const int row = std::min(rows - 1, y / 8);
            const auto first = number(self->cells_.front(), "start");
            const auto last = number(self->cells_.back(), "end");
            const auto unit = first + static_cast<std::uint64_t>(
                static_cast<long double>(row * columns + col) *
                (static_cast<long double>(last) - first + 1.0L) /
                (static_cast<long double>(rows) * columns));
            const auto found = std::lower_bound(self->cells_.begin(), self->cells_.end(), unit,
                [](const Json& cell, std::uint64_t needle) { return number(cell, "end") < needle; });
            if (found == self->cells_.end() || number(*found, "start") > unit) return FALSE;
            const std::string detail = "Unit " + std::to_string(unit) + " · cell " +
                std::to_string(number(*found, "start")) + "–" + std::to_string(number(*found, "end")) +
                " · used " + std::to_string(number(*found, "used")) +
                " · free " + std::to_string(number(*found, "free"));
            gtk_tooltip_set_text(tooltip, detail.c_str());
            return TRUE;
        }), this);
        auto* legend = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(legend),
            "<span foreground='#1267BD'>■</span> Used   "
            "<span foreground='#FF253C'>■</span> Fragmented   "
            "<span foreground='#9E2BFA'>■</span> Directory   "
            "<span foreground='#05214A'>■</span> Free   "
            "<span foreground='#FF9F0A'>■</span> Metadata / reserved");
        gtk_label_set_xalign(GTK_LABEL(legend), 0);
        gtk_box_pack_start(GTK_BOX(map_box), legend, FALSE, FALSE, 0);
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
        if (theme_settings_) {
            g_signal_handler_disconnect(theme_settings_, theme_signal_);
            g_signal_handler_disconnect(theme_settings_, dark_signal_);
        }
        if (auth_timer_) g_source_remove(auth_timer_);
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
        if (discovery_) { g_subprocess_force_exit(discovery_); g_object_unref(discovery_); }
        if (probe_) { g_subprocess_force_exit(probe_); g_object_unref(probe_); }
        if (probe_timer_) g_source_remove(probe_timer_);
        if (discovery_probe_) { g_subprocess_force_exit(discovery_probe_); g_object_unref(discovery_probe_); }
    }

private:
    GtkWidget *window_{}, *volumes_widget_{}, *detail_{}, *volume_title_{}, *progress_{}, *status_{};
    GtkWidget *map_{}, *summary_{}, *log_{}, *analyse_{}, *unmount_{};
    GtkWidget *defrag_{}, *growth_{}, *recover_{}, *stop_{};
    GtkWidget *refresh_{}, *image_{};
    GtkWidget* theme_button_{};
    GtkSettings* theme_settings_ = nullptr;
    gulong theme_signal_ = 0, dark_signal_ = 0;
    GtkWidget* cards_[4]{};
    std::string theme_mode_ = "system";
    std::vector<DesktopVolume> volumes_;
    std::vector<Json> cells_;
    std::unordered_map<std::string, Json> map_cache_;
    std::deque<DesktopVolume> candidates_;
    std::string mapper_, engine_, helper_path_, pending_program_, purpose_, output_, result_status_;
    std::vector<std::string> pending_args_;
    Json map_data_;
    GSubprocess* helper_ = nullptr;
    GSubprocess* local_ = nullptr;
    GSubprocess* discovery_ = nullptr;
    GSubprocess* probe_ = nullptr;
    GSubprocess* discovery_probe_ = nullptr;
    GDataInputStream* helper_input_ = nullptr;
    bool helper_ready_ = false, busy_ = false, stopping_ = false, closing_ = false, discovering_ = false;
    int request_id_ = 0, active_id_ = 0;
    guint auth_timer_ = 0;
    guint probe_timer_ = 0;
    unsigned probe_generation_ = 0;
    fs::path pending_image_;

    fs::path theme_path() const { return fs::path(g_get_user_config_dir()) / "linux-defragger" / "theme"; }
    void load_theme() {
        std::ifstream input(theme_path());
        std::string setting;
        if (input >> setting && (setting == "day" || setting == "night")) theme_mode_ = setting;
    }
    void apply_theme() {
        bool dark = theme_mode_ == "night";
        if (theme_mode_ == "system") {
            auto* settings = gtk_settings_get_default();
            gboolean prefer_dark = FALSE;
            gchar* theme_name = nullptr;
            if (settings) g_object_get(settings, "gtk-application-prefer-dark-theme", &prefer_dark,
                "gtk-theme-name", &theme_name, nullptr);
            gchar* lower = theme_name ? g_ascii_strdown(theme_name, -1) : nullptr;
            dark = prefer_dark || (lower && g_strrstr(lower, "dark"));
            g_free(lower);
            g_free(theme_name);
        }
        install_style(dark);
        if (theme_button_) gtk_button_set_label(GTK_BUTTON(theme_button_),
            theme_mode_ == "system" ? "Theme: Follow system" :
            theme_mode_ == "day" ? "Theme: Day" : "Theme: Night");
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
    static void clicked(GtkButton* button, gpointer) {
        auto* self = owner(GTK_WIDGET(button));
        if (self) self->action(static_cast<const char*>(g_object_get_data(G_OBJECT(button), "action")));
    }
    static void selected(GtkComboBox* combo, gpointer data) {
        if (gtk_combo_box_get_active(combo) >= 0) {
            auto* self = static_cast<Desktop*>(data);
            const auto* volume = self->current();
            const auto found = self->map_cache_.find(self->cache_key(*volume));
            if (found != self->map_cache_.end()) {
                self->map_data_ = found->second;
                self->cells_ = found->second.at("cells").array();
                self->show_metrics(self->map_data_);
            } else {
                self->cells_.clear();
                self->map_data_ = Json();
                for (auto* card : self->cards_) gtk_label_set_text(GTK_LABEL(card), "—");
                gtk_label_set_text(GTK_LABEL(self->summary_), "Run Analyse to inspect the allocation map.");
            }
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
        gtk_main_quit();
        return FALSE;
    }
    DesktopVolume* current() {
        int index = gtk_combo_box_get_active(GTK_COMBO_BOX(volumes_widget_));
        return index >= 0 && static_cast<size_t>(index) < volumes_.size() ? &volumes_[index] : nullptr;
    }
    std::string cache_key(const DesktopVolume& volume) const {
        return volume.path + "|" + volume.filesystem_uuid + "|" + volume.partition_uuid +
            "|" + std::to_string(volume.size);
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
        std::error_code journal_error;
        const bool journal = v && fs::exists(defragger::desktop_journal(*v, getuid()), journal_error);
        auto state = defragger::desktop_controls(v, busy_, stopping_, journal);
        if (journal_error) state.defrag = state.growth_defrag = state.recover = false;
        gtk_widget_set_sensitive(analyse_, state.analyse);
        gtk_widget_set_sensitive(unmount_, state.unmount);
        gtk_widget_set_sensitive(defrag_, state.defrag);
        gtk_widget_set_sensitive(growth_, state.growth_defrag);
        gtk_widget_set_sensitive(recover_, state.recover);
        gtk_widget_set_sensitive(stop_, state.stop);
        gtk_widget_set_sensitive(refresh_, !busy_ && !discovering_);
        gtk_widget_set_sensitive(image_, !busy_ && !probe_);
        gtk_widget_set_sensitive(volumes_widget_, !busy_);
    }
    void refresh() {
        if (busy_ || discovering_) return;
        ++probe_generation_;
        map_cache_.clear();
        candidates_.clear();
        if (probe_timer_) { g_source_remove(probe_timer_); probe_timer_ = 0; }
        if (discovery_probe_) { g_subprocess_force_exit(discovery_probe_); }
        const char* arguments[] = {"lsblk", "--json", "--bytes", "--output",
            "NAME,PATH,TYPE,FSTYPE,FSVER,LABEL,PARTLABEL,UUID,PARTUUID,SIZE,MOUNTPOINTS,RM,RO,MODEL,TRAN", nullptr};
        GError* failure = nullptr;
        discovery_ = g_subprocess_newv(arguments,
            static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &failure);
        if (!discovery_) {
            error("Unable to discover volumes", failure ? failure->message : "lsblk could not start");
            if (failure) g_error_free(failure);
            return;
        }
        discovering_ = true;
        update();
        gtk_label_set_text(GTK_LABEL(status_), "Refreshing volumes…");
        g_subprocess_communicate_utf8_async(discovery_, nullptr, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Desktop*>(data);
                gchar *out = nullptr, *err = nullptr;
                GError* read_error = nullptr;
                const bool complete = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                    &out, &err, &read_error);
                self->discovering_ = false;
                try {
                    if (!complete || g_subprocess_get_exit_status(self->discovery_) != 0)
                        throw std::runtime_error(read_error ? read_error->message : (err ? err : "lsblk failed"));
                    self->apply_discovery(Json::parse(out ? out : ""));
                    gtk_label_set_text(GTK_LABEL(self->status_), "Ready");
                } catch (const std::exception& ex) { self->error("Unable to discover volumes", ex.what()); }
                if (read_error) g_error_free(read_error);
                g_free(out); g_free(err);
                g_object_unref(self->discovery_);
                self->discovery_ = nullptr;
                self->update();
            }, this);
    }
    void apply_discovery(const Json& lsblk) {
            const std::string selected_path = current() ? current()->path : "";
            std::vector<DesktopVolume> previous = volumes_;
            std::vector<DesktopVolume> images;
            for (const auto& v : previous) if (v.image) images.push_back(v);
            volumes_ = defragger::desktop_discover(lsblk);
            for (auto& volume : volumes_) for (const auto& old : previous) {
                if (old.image || !old.verified || old.path != volume.path || old.size != volume.size) continue;
                if ((!old.filesystem_uuid.empty() && old.filesystem_uuid == volume.filesystem_uuid) ||
                    (!old.partition_uuid.empty() && old.partition_uuid == volume.partition_uuid)) {
                    volume.filesystem = old.filesystem;
                    volume.verified = true;
                }
            }
            volumes_.insert(volumes_.end(), images.begin(), images.end());
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(volumes_widget_));
            int preferred = 0;
            for (size_t i = 0; i < volumes_.size(); ++i) {
                const auto& v = volumes_[i];
                std::string label = v.path + " — " + v.filesystem + " — " + bytes(v.size);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(volumes_widget_), label.c_str());
                if (v.path == selected_path) preferred = static_cast<int>(i);
            }
            if (!volumes_.empty()) gtk_combo_box_set_active(GTK_COMBO_BOX(volumes_widget_), preferred);
            else gtk_label_set_text(GTK_LABEL(detail_), "No supported volumes found. Open a filesystem image to begin.");
            auto candidates = defragger::desktop_probe_candidates(lsblk);
            for (auto& candidate : candidates) {
                if (candidates_.size() >= 64) break;
                if (access(candidate.path.c_str(), R_OK) == 0) candidates_.push_back(std::move(candidate));
            }
            begin_next_probe();
            update();
    }
    void begin_next_probe() {
        if (discovery_probe_ || candidates_.empty()) return;
        const auto& pending_candidate = candidates_.front();
        const char* arguments[] = {mapper_.c_str(), pending_candidate.path.c_str(), "--probe", nullptr, nullptr, nullptr};
        if (!pending_candidate.filesystem.empty()) {
            arguments[3] = "--fstype";
            arguments[4] = pending_candidate.filesystem.c_str();
        }
        GError* failure = nullptr;
        discovery_probe_ = g_subprocess_newv(arguments,
            static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &failure);
        if (!discovery_probe_) {
            if (failure) g_error_free(failure);
            candidates_.pop_front();
            begin_next_probe();
            return;
        }
        probe_timer_ = g_timeout_add(2000, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Desktop*>(data);
            self->probe_timer_ = 0;
            if (self->discovery_probe_) g_subprocess_force_exit(self->discovery_probe_);
            return G_SOURCE_REMOVE;
        }, this);
        auto* probe_request = new std::pair<Desktop*, unsigned>(this, probe_generation_);
        g_subprocess_communicate_utf8_async(discovery_probe_, nullptr, nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<std::pair<Desktop*, unsigned>> ctx(
                    static_cast<std::pair<Desktop*, unsigned>*>(data));
                auto* self = ctx->first;
                const bool current_generation = ctx->second == self->probe_generation_;
                gchar *out = nullptr, *err = nullptr;
                GError* read_error = nullptr;
                const bool complete = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                    &out, &err, &read_error);
                if (self->probe_timer_) { g_source_remove(self->probe_timer_); self->probe_timer_ = 0; }
                if (current_generation && !self->candidates_.empty() && complete &&
                    g_subprocess_get_exit_status(self->discovery_probe_) == 0) {
                    try {
                        const Json parsed = Json::parse(out ? out : "");
                        const std::string detected = field(parsed, "filesystem");
                        DesktopVolume proved_candidate = self->candidates_.front();
                        if (defragger::backend_by_fstype(detected) &&
                            (proved_candidate.filesystem.empty() ||
                             defragger::backend_by_fstype(proved_candidate.filesystem) == defragger::backend_by_fstype(detected))) {
                            proved_candidate.filesystem = detected;
                            proved_candidate.verified = true;
                            const auto existing = std::find_if(self->volumes_.begin(), self->volumes_.end(),
                                [&](const DesktopVolume& volume) { return volume.path == proved_candidate.path; });
                            if (existing == self->volumes_.end()) {
                                self->volumes_.push_back(proved_candidate);
                                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(self->volumes_widget_),
                                    (proved_candidate.path + " — " + detected + " — " + bytes(proved_candidate.size)).c_str());
                                if (gtk_combo_box_get_active(GTK_COMBO_BOX(self->volumes_widget_)) < 0)
                                    gtk_combo_box_set_active(GTK_COMBO_BOX(self->volumes_widget_),
                                        static_cast<int>(self->volumes_.size() - 1));
                            } else {
                                existing->filesystem = detected;
                                existing->verified = true;
                            }
                        }
                    } catch (const std::exception& ex) { self->note(std::string("Native discovery probe: ") + ex.what()); }
                }
                if (read_error) g_error_free(read_error);
                g_free(out); g_free(err);
                g_object_unref(self->discovery_probe_);
                self->discovery_probe_ = nullptr;
                if (current_generation && !self->candidates_.empty()) self->candidates_.pop_front();
                self->update();
                self->begin_next_probe();
            }, probe_request);
    }
    void open_image() {
        if (busy_ || probe_) return;
        auto* chooser = gtk_file_chooser_dialog_new("Open filesystem image", GTK_WINDOW(window_),
            GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_ACCEPT, nullptr);
        if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
            char* name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
            try {
                pending_image_ = fs::canonical(name);
                if (!fs::is_regular_file(pending_image_)) throw std::runtime_error("Select a regular filesystem image.");
                const std::string filename = pending_image_.string();
                const char* arguments[] = {mapper_.c_str(), filename.c_str(), "--probe", nullptr};
                GError* failure = nullptr;
                probe_ = g_subprocess_newv(arguments,
                    static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &failure);
                if (!probe_) {
                    const std::string detail = failure ? failure->message : "Native filesystem probe could not start";
                    if (failure) g_error_free(failure);
                    throw std::runtime_error(detail);
                }
                gtk_label_set_text(GTK_LABEL(status_), "Identifying filesystem image…");
                update();
                g_subprocess_communicate_utf8_async(probe_, nullptr, nullptr,
                    [](GObject* source, GAsyncResult* result, gpointer data) {
                        auto* self = static_cast<Desktop*>(data);
                        gchar *out = nullptr, *err = nullptr;
                        GError* probe_error = nullptr;
                        const bool complete = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                            &out, &err, &probe_error);
                        try {
                            if (!complete || g_subprocess_get_exit_status(self->probe_) != 0)
                                throw std::runtime_error(probe_error ? probe_error->message : (err ? err : "Unrecognised filesystem image"));
                            Json parsed = Json::parse(out ? out : "");
                            DesktopVolume volume;
                            volume.path = self->pending_image_.string();
                            volume.filesystem = field(parsed, "filesystem");
                            if (!defragger::backend_by_fstype(volume.filesystem))
                                throw std::runtime_error("No native backend recognised this filesystem image.");
                            volume.size = fs::file_size(self->pending_image_);
                            volume.readonly = access(volume.path.c_str(), W_OK) != 0;
                            volume.image = volume.verified = true;
                            self->volumes_.push_back(volume);
                            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(self->volumes_widget_),
                                (volume.path + " — " + volume.filesystem + " — " + bytes(volume.size)).c_str());
                            gtk_combo_box_set_active(GTK_COMBO_BOX(self->volumes_widget_),
                                static_cast<int>(self->volumes_.size() - 1));
                            gtk_label_set_text(GTK_LABEL(self->status_), "Image ready");
                        } catch (const std::exception& ex) { self->error("Unable to open image", ex.what()); }
                        if (probe_error) g_error_free(probe_error);
                        g_free(out); g_free(err);
                        g_object_unref(self->probe_);
                        self->probe_ = nullptr;
                        self->update();
                    }, this);
            } catch (const std::exception& ex) { error("Unable to open image", ex.what()); }
            g_free(name);
        }
        gtk_widget_destroy(chooser);
    }
    void action(const std::string& action_name) {
        if (action_name == "theme") {
            theme_mode_ = theme_mode_ == "system" ? "day" : theme_mode_ == "day" ? "night" : "system";
            std::error_code ec;
            fs::create_directories(theme_path().parent_path(), ec);
            if (!ec) { std::ofstream output(theme_path()); output << theme_mode_ << '\n'; }
            apply_theme();
            return;
        }
        if (action_name == "minimize") { gtk_window_iconify(GTK_WINDOW(window_)); return; }
        if (action_name == "maximize") {
            if (gtk_window_is_maximized(GTK_WINDOW(window_))) gtk_window_unmaximize(GTK_WINDOW(window_));
            else gtk_window_maximize(GTK_WINDOW(window_));
            return;
        }
        if (action_name == "close") { gtk_window_close(GTK_WINDOW(window_)); return; }
        if (busy_ && action_name != "stop" && action_name != "about" &&
            action_name != "test-media") return;
        if (action_name == "refresh") { refresh(); return; }
        if (action_name == "image") { open_image(); return; }
        if (action_name == "stop") { request_stop(); return; }
        if (action_name == "about") {
            auto* about = gtk_about_dialog_new();
            gtk_window_set_transient_for(GTK_WINDOW(about), GTK_WINDOW(window_));
            gtk_window_set_modal(GTK_WINDOW(about), TRUE);
            gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about), "Defragmenter");
            gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about), LD_VERSION);
            gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about),
                "Native filesystem allocation analysis and safe offline defragmentation.");
            gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(about),
                "Copyright © 1993–2026 Shannon Smith");
            gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about),
                "https://github.com/Infiltrator-Projects/Defragmenter");
            gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
            const char* authors[] = {"Shannon Smith — Author and project maintainer", nullptr};
            gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(about), authors);
            const fs::path icon_path = fs::is_regular_file("/usr/lib/linux-defragger/defragmenter-icon.png")
                ? "/usr/lib/linux-defragger/defragmenter-icon.png"
                : "defragger/packaging/io.github.linuxdefragger.png";
            auto* logo = gdk_pixbuf_new_from_file_at_scale(icon_path.c_str(), 96, 96, TRUE, nullptr);
            if (logo) { gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(about), logo); g_object_unref(logo); }
            gtk_dialog_run(GTK_DIALOG(about));
            gtk_widget_destroy(about);
            return;
        }
        if (action_name == "test-media") {
            GError* failure = nullptr;
            const char* argv[] = {"linux-defragger-test-media", nullptr};
            auto* process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, &failure);
            if (process) g_object_unref(process);
            else { error("Unable to launch Test Media", failure->message); g_error_free(failure); }
            return;
        }
        auto* v = current();
        if (!v || busy_) return;
        if (action_name == "analyse") {
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
                map_cache_.erase(cache_key(*v));
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
                    if (self->busy_ && !self->local_) {
                        if (self->stopping_ && !self->active_id_) {
                            self->result_status_ = "stopped";
                            self->completed(0, "Cancelled before the operation started");
                        } else self->completed(127, detail);
                    }
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
            note("Stopping read-only analysis…");
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
        if (code == 130 && (purpose == "defrag" || purpose == "growth-defrag" || purpose == "recover"))
            result_status_ = "stopped";
        bool actually_started = active_id_ != 0 || local_ != nullptr;
        active_id_ = 0;
        busy_ = false; stopping_ = false;
        if (code == 0 && purpose == "analysis") {
            try { present_map(Json::parse(output_)); }
            catch (const std::exception& ex) { code = 127; reason = std::string("Invalid allocation map: ") + ex.what(); }
        }
        if (code == 0 && purpose == "unmount") refresh();
        const bool success = code == 0 && result_status_ != "failed" && result_status_ != "stopped";
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_),
            success || result_status_ == "stopped" ? 1.0 : 0.0);
        gtk_label_set_text(GTK_LABEL(status_), success ?
            (result_status_ == "not-needed" ? "No changes needed" : "Completed") :
            (result_status_ == "stopped" ? "Stopped safely" : "Operation failed"));
        if (code != 0 && result_status_ != "stopped") error("Operation failed", reason.empty() ? "Exit status " + std::to_string(code) : reason);
        if (code == 0 && result_status_ == "failed") error("Operation failed", reason.empty() ? "Native worker reported failure" : reason);
        if (!closing_ && actually_started && (success || result_status_ == "stopped") &&
            (purpose == "defrag" || purpose == "growth-defrag" || purpose == "recover")) {
            update();
            action("analyse");
        }
        update();
        if (closing_ && !busy_) gtk_widget_destroy(window_);
    }
    void present_map(const Json& map) {
        auto validated = defragger::desktop_validate_map(map);
        auto* v = current();
        if (!v) return;
        defragger::desktop_verify_identity(*v, field(map, "filesystem"));
        map_data_ = map;
        cells_ = std::move(validated);
        map_cache_[cache_key(*v)] = map;
        gtk_widget_queue_draw(map_);
        show_metrics(map);
    }
    void show_metrics(const Json& map) {
        std::string summary = "Capacity " + bytes(number(map, "total_bytes")) + "    Free "
            + bytes(number(map, "free_bytes")) + "    Used " + bytes(number(map, "used_bytes"));
        gtk_label_set_text(GTK_LABEL(cards_[0]), map.find("fragmented_files") ?
            (std::to_string(number(map, "fragmented_files")) + " files").c_str() : "Not calculated");
        gtk_label_set_text(GTK_LABEL(cards_[1]), bytes(number(map, "free_bytes")).c_str());
        gtk_label_set_text(GTK_LABEL(cards_[2]), bytes(number(map, "used_bytes")).c_str());
        gtk_label_set_text(GTK_LABEL(cards_[3]), map.find("regular_files") ?
            (std::to_string(number(map, "regular_files")) + " files").c_str() : "Unknown");
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
        cairo_set_source_rgb(cr, 0.09, 0.13, 0.19);
        cairo_paint(cr);
        if (self->cells_.empty()) return FALSE;
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
            if (number(cell, "bad")) cairo_set_source_rgb(cr, 1.0, 0.625, 0.040);
            else if (number(cell, "fragmented")) cairo_set_source_rgb(cr, 1.0, 0.145, 0.235);
            else if (number(cell, "directory")) cairo_set_source_rgb(cr, 0.62, 0.17, 0.98);
            else if (number(cell, "used")) cairo_set_source_rgb(cr, 0.020, 0.520, 1.0);
            else if (number(cell, "free")) cairo_set_source_rgb(cr, 0.018, 0.050, 0.105);
            else if (number(cell, "outside")) cairo_set_source_rgb(cr, 0.006, 0.014, 0.030);
            else cairo_set_source_rgb(cr, 0.285, 0.330, 0.410);
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
