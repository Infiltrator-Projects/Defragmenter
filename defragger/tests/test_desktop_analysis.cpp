// SPDX-License-Identifier: GPL-3.0-or-later
#define LD_DESKTOP_ANALYSIS_TEST 1
#include "../native/desktop.cpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(1); } } while (0)

namespace {
struct DesktopAnalysisTest {
    Desktop desktop{false};
    unsigned int phase = 0U;
    unsigned int ticks = 0U;
    static constexpr const char* map =
        "{\"filesystem\":\"ntfs\",\"map_accuracy\":\"exact\",\"cell_count\":1,"
        "\"total_units\":8,\"unit_size\":512,\"total_bytes\":4096,\"unknown_bytes\":0,"
        "\"free_bytes\":512,\"used_bytes\":3584,\"regular_files\":2,\"fragmented_files\":1,"
        "\"defrag_qualified\":false,\"growth_qualified\":false,"
        "\"cells\":[{\"start\":0,\"end\":7,\"used\":7,\"free\":1}]}";

    DesktopAnalysisTest() {
        DesktopVolume volume;
        volume.path = "/dev/null"; volume.filesystem = "ntfs";
        volume.size = 4096U; volume.image = true; volume.verified = true;
        desktop.volumes_.push_back(volume);
        g_signal_handlers_block_by_func(desktop.volumes_widget_, reinterpret_cast<gpointer>(Desktop::selected), &desktop);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(desktop.volumes_widget_), "Read-only test fixture");
        gtk_combo_box_set_active(GTK_COMBO_BOX(desktop.volumes_widget_), 0);
        g_signal_handlers_unblock_by_func(desktop.volumes_widget_, reinterpret_cast<gpointer>(Desktop::selected), &desktop);
        const std::string command =
            "printf '%s\\n' '@@ANALYSIS {\"phase\":\"Scanning NTFS MFT\",\"completed\":32,\"total\":64}' >&2; "
            "sleep 1; printf '%s\\n' '" + std::string(map) + "'";
        desktop.start({"/bin/sh", "-c", command}, "mapper", "analysis");
        CHECK(desktop.map_data_.is_null());
        CHECK(desktop.cells_.empty());
        CHECK(std::string(gtk_label_get_text(GTK_LABEL(desktop.summary_))).find("Run Analyse") == std::string::npos);
    }

    static gboolean probe(gpointer data) {
        auto* self = static_cast<DesktopAnalysisTest*>(data);
        if (++self->ticks >= 100U) {
            std::fprintf(stderr, "analysis test timed out: phase=%u busy=%d progress=%s result=%s\n",
                self->phase, self->desktop.busy_, self->desktop.analysis_phase_.c_str(),
                self->desktop.result_status_.c_str());
            CHECK(false);
        }
        auto& app = self->desktop;
        if (self->phase == 0U) {
            if (app.analysis_phase_.find("32 / 64 records") == std::string::npos) return G_SOURCE_CONTINUE;
            CHECK(app.busy_ && app.local_ != nullptr);
            CHECK(app.output_.empty()); /* Progress arrived before the final JSON. */
            CHECK(gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(app.progress_)) == 0.5);
            CHECK(std::string(gtk_label_get_text(GTK_LABEL(app.summary_))).find("elapsed") != std::string::npos);
            ++self->phase;
            return G_SOURCE_CONTINUE;
        }
        if (self->phase == 1U) {
            if (app.busy_) return G_SOURCE_CONTINUE;
            CHECK(app.local_ == nullptr && app.analysis_timer_ == 0U);
            CHECK(app.cells_.size() == 1U);
            CHECK(!gtk_widget_get_sensitive(app.defrag_));
            CHECK(!gtk_widget_get_sensitive(app.growth_));
            CHECK(std::string(gtk_label_get_text(GTK_LABEL(app.status_))) == "Completed");
            /* Administrator transport must also keep events out of map JSON. */
            app.busy_ = true; app.purpose_ = "analysis"; app.active_id_ = 7;
            app.output_.clear();
            app.receive(Json::Object{{"type", Json("output")}, {"id", Json::integer(7)},
                {"line", Json("@@ANALYSIS {\"phase\":\"Checking availability\"}")}});
            CHECK(app.output_.empty());
            CHECK(app.analysis_phase_ == "Checking availability");
            app.busy_ = false; app.active_id_ = 0;
            app.start({"/bin/sh", "-c", "printf '%s\\n' '@@ANALYSIS {\"phase\":\"Reading metadata\"}' >&2; sleep 10"}, "mapper", "analysis");
            app.request_stop();
            ++self->phase;
            return G_SOURCE_CONTINUE;
        }
        if (app.busy_) return G_SOURCE_CONTINUE;
        CHECK(app.local_ == nullptr && app.analysis_timer_ == 0U);
        CHECK(app.result_status_ == "stopped");
        CHECK(app.cells_.empty());
        CHECK(!gtk_widget_get_sensitive(app.defrag_));
        gtk_widget_destroy(app.window_);
        ++self->phase;
        return G_SOURCE_REMOVE;
    }
};
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    DesktopAnalysisTest test;
    g_timeout_add(50U, DesktopAnalysisTest::probe, &test);
    gtk_main();
    CHECK(test.phase == 3U);
    return 0;
}
