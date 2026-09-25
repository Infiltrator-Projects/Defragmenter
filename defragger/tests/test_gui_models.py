#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Functional tests for the GUI's non-GTK module boundaries."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from core.protocol import EngineEventParser, OperationResult
from ui.backend_catalog import BackendCatalog
from ui.devices import Volume
from ui.map_geometry import allocation_grid, block_bounds, source_cell_at
from ui.map_presenter import AllocationMapError, present_allocation_map
from ui.live_controller import LiveEventController
from ui.operation_status import successful_completion


def _manifest(backend_id: str = "ext4", alias: str = "ext3") -> dict:
    return {
        "schema": 2,
        "backends": [
            {
                "id": backend_id,
                "aliases": [alias],
                "capabilities": 0,
                "operations": [
                    {"name": "defrag", "label": "Defragment"},
                    {"name": "growth-defrag", "label": "Growth Defrag"},
                    {"name": "recover", "label": "Recover"},
                ],
            }
        ],
    }


def _cells() -> list[dict[str, int]]:
    return [
        {
            "start": 0,
            "end": 3,
            "free": 1,
            "used": 3,
            "fragmented": 2,
            "directory": 1,
        },
        {
            "start": 4,
            "end": 7,
            "free": 4,
            "used": 0,
            "fragmented": 0,
            "directory": 0,
        },
    ]


def test_catalog_is_validated_immutable_and_instance_owned() -> None:
    first = BackendCatalog.from_manifest(_manifest())
    second = BackendCatalog.from_manifest(_manifest("xfs", "xfs-test"))
    assert first.normalize("EXT3") == "ext4"
    assert set(first.operations_for("ext3")) == {"defrag", "growth-defrag", "recover"}
    assert set(first.operations_for("ext4")) == {
        "defrag",
        "growth-defrag",
        "recover",
    }
    try:
        first.aliases["new"] = "ext4"  # type: ignore[index]
    except TypeError:
        pass
    else:
        raise AssertionError("backend catalogue is mutable")

    ext_volume = Volume(
        catalog=first,
        path="/dev/ext",
        name="ext",
        fstype="ext3",
        label="",
        size=1024,
        mountpoints=[],
        removable=False,
        readonly=False,
        model="",
        transport="",
    )
    xfs_volume = Volume(
        catalog=second,
        path="/dev/xfs",
        name="xfs",
        fstype="xfs-test",
        label="",
        size=1024,
        mountpoints=[],
        removable=False,
        readonly=False,
        model="",
        transport="",
    )
    assert ext_volume.normalized_fstype == "ext4"
    assert xfs_volume.normalized_fstype == "xfs"

    ambiguous = {
        "backends": [
            {"id": "one", "aliases": ["same"], "operations": []},
            {"id": "two", "aliases": ["same"], "operations": []},
        ]
    }
    try:
        BackendCatalog.from_manifest(ambiguous)
    except ValueError:
        pass
    else:
        raise AssertionError("ambiguous backend alias was accepted")


def test_map_presenter_validates_and_normalises_fat_map() -> None:
    data = {
        "filesystem": "FAT32",
        "volume_id": "1234abcd",
        "cluster_size": 4096,
        "data_clusters": 8,
        "free_clusters": 5,
        "regular_files": 2,
        "directories": 1,
        "fragmented_files": 1,
        "fragmented_directories": 0,
        "free_gaps_below_highest": 2,
        "cell_count": 2,
        "cells": _cells(),
    }
    view = present_allocation_map(data, ("defrag",))
    assert view.capacity_value == "32.0 KB"
    assert view.free_value == "20.0 KB (62.5%)"
    assert view.fragmentation_value == "1 files · 0 dirs"
    assert view.unit_label == "clusters"
    assert view.caption.startswith("Allocation image:")
    assert view.cells[0]["outside"] == 0
    assert view.analysis_log.endswith("1 fragmented files, 0 fragmented directories.")


def test_small_filesystems_use_square_allocation_blocks_not_row_stripes() -> None:
    geometry = allocation_grid(2044, 1190, 260)
    assert geometry.uses_one_block_per_cell
    assert geometry.columns == 97
    assert geometry.rows == 22

    first = block_bounds(geometry, 0)
    last_in_first_row = block_bounds(geometry, geometry.columns - 1)
    first_in_second_row = block_bounds(geometry, geometry.columns)
    assert first[2] - first[0] in (12, 13)
    assert first[3] - first[1] in (11, 12)
    assert last_in_first_row[1] == first[1]
    assert first_in_second_row[1] > first[1]
    assert source_cell_at(geometry, first[0], first[1]) == 0
    assert source_cell_at(
        geometry,
        first_in_second_row[0],
        first_in_second_row[1],
    ) == geometry.columns

    sampled = allocation_grid(300, 10, 10)
    assert not sampled.uses_one_block_per_cell
    assert source_cell_at(sampled, 9, 9) == 297


def test_map_presenter_handles_domain_and_swap_maps() -> None:
    domain = {
        "backend": "read-only-domain",
        "filesystem": "ext4",
        "unit_size": 4096,
        "total_units": 8,
        "cell_count": 2,
        "total_bytes": 8 * 4096,
        "filesystem_bytes": 6 * 4096,
        "outside_bytes": 2 * 4096,
        "free_bytes": 3 * 4096,
        "used_bytes": 3 * 4096,
        "unknown_bytes": 0,
        "cells": _cells(),
    }
    writable = present_allocation_map(
        domain,
        ("defrag", "growth-defrag", "recover"),
    )
    assert writable.fragmentation_value == "Not calculated"
    assert "dark tail" in writable.caption
    assert "available: Defragment, Growth Defrag, Recover" in writable.status

    read_only = present_allocation_map(domain)
    assert read_only.fragmentation_value == "Not available"
    assert "read-only allocation map" in read_only.status

    unknown_cells = [
        {"start": 0, "end": 3, "free": 0, "used": 0, "unknown": 4},
        {"start": 4, "end": 7, "free": 0, "used": 0, "unknown": 4},
    ]
    for filesystem in ("ufs", "zfs"):
        unknown_summary = {
            "backend": "read-only-domain",
            "filesystem": filesystem,
            "map_accuracy": "summary",
            "unit_size": 512,
            "total_units": 8,
            "cell_count": 2,
            "total_bytes": 4096,
            "free_bytes": 0,
            "used_bytes": 0,
            "unknown_bytes": 4096,
            "cells": unknown_cells,
        }
        unknown_view = present_allocation_map(unknown_summary)
        assert unknown_view.free_value == "Unknown"
        assert unknown_view.files_value == "Unknown"
        assert unknown_view.fragmentation_value == "Not available"
        assert "exact allocation totals not decoded" in unknown_view.status
        assert "exact allocated/free totals" in unknown_view.analysis_log
        assert "0 B allocated" not in unknown_view.analysis_log
        assert "0 B free" not in unknown_view.analysis_log

    swap = dict(domain)
    swap.update(
        filesystem="swap",
        filesystem_bytes=8 * 4096,
        outside_bytes=0,
        free_bytes=6 * 4096,
        used_bytes=2 * 4096,
        details={"active": True, "page_size": 4096},
    )
    swap_view = present_allocation_map(swap)
    assert swap_view.files_title == "Usage"
    assert swap_view.files_value == "8.0 KB used · 2 pages"
    assert swap_view.fragmentation_value == "Not applicable"
    assert "aggregate usage only" in swap_view.caption
    assert "2 pages" in swap_view.status
    assert "physical occupied slot locations are unavailable" in swap_view.analysis_log

    swap["used_bytes"] = 4096
    swap["free_bytes"] = 7 * 4096
    one_page = present_allocation_map(swap)
    assert one_page.files_value == "4.0 KB used · 1 page"


def test_xfs_live_strokes_are_preview_then_authoritative() -> None:
    data = {
        "backend": "read-only-domain",
        "filesystem": "xfs",
        "unit_size": 4096,
        "total_units": 8,
        "filesystem_bytes": 8 * 4096,
        "cells": _cells(),
    }
    controller = LiveEventController()

    preview = controller.consume(
        '@@LIVE_RANGES {"ranges":[[0,16384,4096]],'
        '"moved_total_bytes":4096,"pass":1,"objects_done":0,'
        '"objects_total":0,"scope":"stage-preview","sequence":1}',
        data,
        purpose="defrag",
    )
    assert preview.map_changed
    assert preview.draw_immediately
    assert preview.status is not None
    assert "Staging preview" in preview.status
    assert "source filesystem unchanged" in preview.status

    source_reset = controller.consume(
        '@@LIVE_RESET {"scope":"source-authoritative","unit_size":4096,'
        '"filesystem_units":8,"used_ranges":[[0,8192]]}',
        data,
        purpose="defrag",
    )
    assert source_reset.map_changed
    assert source_reset.draw_immediately
    assert source_reset.status is not None
    assert "Authoritative source allocation restored" in source_reset.status
    assert sum(int(cell["used"]) for cell in data["cells"]) == 2

    verified_reset = controller.consume(
        '@@LIVE_RESET {"scope":"verified-authoritative","unit_size":4096,'
        '"filesystem_units":8,"used_ranges":[[0,16384]]}',
        data,
        purpose="defrag",
    )
    assert verified_reset.map_changed
    assert verified_reset.status is not None
    assert "verified source filesystem" in verified_reset.status
    assert sum(int(cell["used"]) for cell in data["cells"]) == 4

    worker_source = (GUI / "filesystems" / "xfs" / "native" / "xfs_worker.c").read_text()
    plan_source = (GUI / "filesystems" / "xfs" / "native" / "xfs_plan.c").read_text()
    assert 'emit_live_reset(&source, "source-authoritative")' in worker_source
    assert 'emit_live_reset(&committed, "verified-authoritative")' in worker_source
    assert 'emit_live_reset(&verified)' not in worker_source
    assert 'stage-preview' in plan_source


def test_invalid_map_is_rejected_before_widget_state_changes() -> None:
    invalid = {
        "filesystem": "fat32",
        "cluster_size": 4096,
        "data_clusters": 8,
        "free_clusters": 1,
        "regular_files": 1,
        "directories": 1,
        "fragmented_files": 0,
        "fragmented_directories": 0,
        "free_gaps_below_highest": 0,
        "cell_count": 3,
        "cells": _cells(),
    }
    try:
        present_allocation_map(invalid)
    except AllocationMapError:
        pass
    else:
        raise AssertionError("map with a false cell count was accepted")


def test_result_protocol_replaces_worker_output_text_matching() -> None:
    event = EngineEventParser.parse(
        '@@RESULT {"operation":"growth-defrag","status":"not-needed","message":""}'
    )
    assert event is not None
    result = event.operation_result()
    presentation = successful_completion("growth-defrag", result)
    assert presentation.progress_text == "Not needed"
    assert presentation.post_analysis_status is not None

    ordinary = successful_completion(
        "growth-defrag",
        OperationResult("growth-defrag", "completed"),
    )
    assert ordinary.progress_text == "Complete"

    failed_event = EngineEventParser.parse(
        '@@RESULT {"operation":"defrag","status":"failed","message":"unsupported layout"}'
    )
    assert failed_event is not None
    assert failed_event.operation_result().status == "failed"

    try:
        EngineEventParser.parse(
            '@@RESULT {"operation":"growth-defrag","status":"maybe"}'
        )
    except ValueError:
        pass
    else:
        raise AssertionError("invalid worker result status was accepted")

    controller_source = (GUI / "ui" / "command_runner.py").read_text()
    assert "Growth Defrag status:          Not needed;" not in controller_source


def test_about_dialog_matches_the_standard_project_identity() -> None:
    view_source = (GUI / "ui" / "window_view.py").read_text()
    about_source = (GUI / "ui" / "about.py").read_text()
    for required in (
        'APP_ICON_NAME = "io.github.linuxdefragger"',
        'COPYRIGHT = "Copyright © 1993-2026 Shannon Smith"',
        'PROJECT_URL = "https://github.com/Infiltrator-Projects/Defragmenter"',
        "ABOUT_LICENSE",
        "COPYING.GPL-3.0",
    ):
        assert required in view_source

    for required in (
        "class SuiteStandardWindowView(WindowView):",
        "Gtk.AboutDialog(",
        "program_name=info.product_name",
        "version=info.version",
        'website_label="Website"',
        "dialog.set_authors(list(info.authors))",
        "dialog.set_license(info.license_text)",
        "dialog.set_logo_icon_name(None)",
        "dialog.set_logo(logo)",
        "build=self.build_label",
        "Shannon Smith — Author and project maintainer",
    ):
        assert required in about_source

    for forbidden in (
        "dialog.set_default_size(",
        "dialog.set_size_request(",
        'subtitle="DEFRAGMENTER · NATIVE FILESYSTEM OPTIMISATION"',
        'website_label="Project website"',
        'add_class("link-about-dialog")',
    ):
        assert forbidden not in about_source

    # Keep one concrete GTK About implementation. The base view retains only
    # the dispatch point used by its menu construction.
    assert "Gtk.AboutDialog(" not in view_source
    assert "Gtk.LinkButton.new_with_label(PROJECT_URL" not in view_source
    assert "def _show_license(" not in view_source
    assert about_source.count("def show_about(self) -> None:") == 1

    version_template = (ROOT / "packaging" / "generated" / "version.py.in").read_text()
    assert 'BUILD_PROFILE = "@LINUX_DEFRAGGER_BUILD_PROFILE@"' in version_template
    assert 'BUILD_LABEL = "@LINUX_DEFRAGGER_BUILD_LABEL@"' in version_template
    assert "Operation engine:" not in about_source
    assert "hfsutils" not in about_source
    assert "LICENSES/GPL-3.0-or-later.txt" not in view_source


def test_ui_polish_preserves_allocation_map_visual_contract() -> None:
    source = (GUI / "ui" / "widgets.py").read_text()
    view_source = (GUI / "ui" / "window_view.py").read_text()
    for required in (
        '"free": (0.018, 0.050, 0.105)',
        '"outside": (0.006, 0.014, 0.030)',
        '"used": (0.020, 0.520, 1.000)',
        '"fragmented": (1.000, 0.145, 0.235)',
        '"directory": (0.620, 0.170, 0.980)',
        '"bad": (1.000, 0.625, 0.040)',
        '"background": (0.010, 0.020, 0.040)',
        "self.set_size_request(-1, 250)",
        "def _morton_xy(",
        "def _morton_index(",
        "class GaugeCard(Gtk.Frame):",
        "locality-preserving pixel image",
    ):
        assert required in source

    for required in (
        '"Test Media"',
        '"Settings"',
        "self.controller.launch_test_media()",
        "GaugeCard(",
        "Pixel locality map",
        "locality-preserving pixel view",
    ):
        assert required in view_source

    # The dashboard deliberately visualises the linear disk through a
    # locality-preserving pixel image. It must not regress to visible grid
    # geometry or the old row-major stripe renderer.
    assert '"grid":' not in source
    assert "allocation_grid(" not in source
    assert "block_bounds(" not in source
    assert "pixel_index = y * width + x" not in source


def test_theme_modes_are_persistent_and_shared_across_windows() -> None:
    theme_source = (GUI / "ui" / "theme.py").read_text()
    tokens_source = (GUI / "ui" / "theme_tokens.py").read_text()
    generator_source = (ROOT / "tools" / "update-theme-tokens.py").read_text()
    about_source = (GUI / "ui" / "about.py").read_text()
    test_media_theme = (ROOT / "test_media" / "test_media_theme.c").read_text()
    view_source = (GUI / "ui" / "window_view.py").read_text()
    application_source = (GUI / "ui" / "application.py").read_text()

    for required in (
        'SYSTEM = "system"',
        'DAY = "day"',
        'NIGHT = "night"',
        "def load_theme_mode()",
        "def save_theme_mode(",
        "def apply_theme(",
        '"Follow system"',
        '"Day"',
        '"Night"',
        "XDG_CONFIG_HOME",
        "from .theme_tokens import DAY, METRICS, NIGHT, TYPOGRAPHY",
    ):
        assert required in theme_source

    for forbidden_private_palette in (
        "#050608",
        "#F4F5F7",
        "#D7DDE2",
        "#20252B",
    ):
        assert forbidden_private_palette not in theme_source, (
            f"Defragger reintroduced private theme truth: {forbidden_private_palette}"
        )
    assert "#050608" not in about_source.lower()
    assert "#eef1f3" not in about_source.lower()

    common_design = ROOT / "shared" / "infiltratr-common" / "design" / "infiltrator-design-v1.json"
    assert common_design.is_file()
    assert 'COMMON = ROOT / "shared/infiltratr-common/design/infiltrator-design-v1.json"' in generator_source
    assert "THEME_CONTRACT_VERSION = 1" in tokens_source

    namespace: dict[str, object] = {}
    exec(tokens_source, namespace)
    import json

    design = json.loads(common_design.read_text())
    assert namespace["DAY"] == design["theme"]["palettes"]["day"]
    assert namespace["NIGHT"] == design["theme"]["palettes"]["night"]
    assert namespace["THEME_CONTRACT_VERSION"] == design["theme"]["contract_version"]
    assert namespace["TYPOGRAPHY"]["ui_family"] == design["typography"]["ui_family"]
    assert namespace["TYPOGRAPHY"]["brand_family"] == design["typography"]["brand_family"]
    assert namespace["TYPOGRAPHY"]["ui_bold_weight"] == design["typography"]["ui_bold_weight"]
    assert namespace["TYPOGRAPHY"]["font_files"] == design["typography"]["font_files"]
    assert namespace["METRICS"] == design["metrics"]

    for role in (
        "titlebar",
        "connection",
        "connection_border",
        "heading",
        "summary",
        "kicker",
        "detail_label",
        "note",
        "status_border",
        "accent_hover",
    ):
        assert f'p["{role}"]' in theme_source, (
            f"GTK theme does not consume Common 1.19.27 role {role}"
        )
    for role in (
        "titlebar_rgb",
        "connection_rgb",
        "connection_border_rgb",
        "heading_rgb",
        "summary_rgb",
        "detail_label_rgb",
        "note_rgb",
        "status_border_rgb",
        "accent_hover_rgb",
    ):
        assert f"palette->{role}" in test_media_theme

    assert 'Gtk.MenuItem.new_with_label("Theme")' in view_source
    assert "Gtk.RadioMenuItem" in view_source
    assert "save_theme_mode(mode)" in view_source
    assert "apply_theme(mode)" in view_source
    assert "sync_theme_menu" in view_source
    assert "apply_theme(load_theme_mode())" in application_source

    # Follow-system is policy, not a third palette: host light/dark state
    # must resolve to the exact Common Day or Night palette and update live.
    assert "def _system_prefers_dark()" in theme_source
    assert "notify::gtk-theme-name" in theme_source
    assert "notify::gtk-application-prefer-dark-theme" in theme_source
    assert "resolved is ThemeMode.SYSTEM and _system_prefers_dark()" in theme_source
    assert "_night_css() if effective is ThemeMode.NIGHT else _day_css()" in theme_source


def test_mb_typography_has_no_system_font_escape_hatches() -> None:
    theme_source = (GUI / "ui" / "theme.py").read_text()
    widgets_source = (GUI / "ui" / "widgets.py").read_text()
    tokens_source = (GUI / "ui" / "theme_tokens.py").read_text()
    generator_source = (ROOT / "tools" / "update-theme-tokens.py").read_text()
    test_media_theme = (ROOT / "test_media" / "test_media_theme.c").read_text()
    test_media_gui = (ROOT / "test_media" / "test_media_gui.c").read_text()
    packaging = (ROOT / "packaging" / "build-deb.sh").read_text()
    vendor = (ROOT / "packaging" / "vendor-mb-fonts.sh").read_text()
    vendor_cmake = (ROOT / "packaging" / "vendor-mb-fonts.cmake").read_text()

    assert "TYPOGRAPHY" in theme_source
    assert 'TYPOGRAPHY["ui_family"]' in theme_source
    assert 'TYPOGRAPHY["brand_family"]' in theme_source
    assert 'TYPOGRAPHY["ui_family"]' in widgets_source
    assert "infiltratr_typography()" in test_media_theme
    assert "infiltratr_design_metrics()" in test_media_theme
    assert "infiltratr_theme_resolve" in test_media_theme

    for font_file in (
        "mb_corpo_a_cond_regular.ttf",
        "mb_corpo_s_bold.ttf",
        "mb_corpo_s_regular.ttf",
    ):
        assert font_file in packaging
        assert font_file in tokens_source

    assert "vendor-mb-fonts.cmake" in vendor
    assert "python3" not in vendor
    assert "InfiltratrTypographyAssets.cmake" in vendor_cmake
    assert "INFILTRATR_MB_CORPO_ARCHIVE_URL" in vendor_cmake
    assert "INFILTRATR_MB_CORPO_ARCHIVE_SHA256" in vendor_cmake
    assert "raw.githubusercontent.com" not in vendor_cmake
    assert 'data["typography"]' in generator_source

    for source in (theme_source, test_media_theme):
        assert "Sans" not in source
        assert "system-ui" not in source
        assert "monospace" not in source.lower()

    assert "gtk_text_view_set_monospace" not in test_media_gui
    assert "fc-scan" not in theme_source
    assert "fc-scan" not in test_media_theme

def test_main_window_remains_resizable_maximisable_and_workarea_bounded() -> None:
    window_source = (GUI / "ui" / "window.py").read_text()
    view_source = (GUI / "ui" / "window_view.py").read_text()
    widgets_source = (GUI / "ui" / "widgets.py").read_text()

    assert "self.set_decorated(True)" in window_source
    assert "self.set_resizable(True)" in window_source
    assert "self.set_type_hint(Gdk.WindowTypeHint.NORMAL)" in window_source
    assert "self.set_default_size(1480, 900)" in window_source
    assert "self.set_default_size(1040, 680)" not in window_source
    assert "self.set_resizable(False)" not in window_source
    assert "set_geometry_hints" not in window_source
    assert 'self.connect("realize", self._configure_native_window)' in window_source
    for function in (
        "Gdk.WMFunction.RESIZE",
        "Gdk.WMFunction.MOVE",
        "Gdk.WMFunction.MINIMIZE",
        "Gdk.WMFunction.MAXIMIZE",
        "Gdk.WMFunction.CLOSE",
    ):
        assert function in window_source
    assert "Gdk.WMFunction.ALL" not in window_source
    assert "monitor.get_workarea()" in window_source
    assert "workarea.width - 48" in window_source
    assert "workarea.height - 64" in window_source
    assert "self.resize(width, height)" in window_source
    assert "body_scroll = Gtk.ScrolledWindow()" in view_source
    assert "body_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)" in view_source
    assert "body_scroll.add(root)" in view_source
    assert "scroll.set_min_content_height(100)" in view_source
    assert "self.set_size_request(-1, 250)" in widgets_source
    assert "self.set_size_request(640, 260)" not in widgets_source
    assert "MAP_RESIZE_DEBOUNCE_MS = 180" in window_source
    assert "self.coordinator.desired_map_cells(" in window_source
    assert "_allocation.width" in window_source
    assert "_allocation.height" in window_source
    assert "self.coordinator.map_resolution_needs_refresh(target)" in window_source
    assert "target_cells=target" in window_source
    assert "quiet=True" in window_source


def main() -> None:
    test_catalog_is_validated_immutable_and_instance_owned()
    test_map_presenter_validates_and_normalises_fat_map()
    test_small_filesystems_use_square_allocation_blocks_not_row_stripes()
    test_map_presenter_handles_domain_and_swap_maps()
    test_xfs_live_strokes_are_preview_then_authoritative()
    test_invalid_map_is_rejected_before_widget_state_changes()
    test_result_protocol_replaces_worker_output_text_matching()
    test_about_dialog_matches_the_standard_project_identity()
    test_ui_polish_preserves_allocation_map_visual_contract()
    test_theme_modes_are_persistent_and_shared_across_windows()
    test_mb_typography_has_no_system_font_escape_hatches()
    test_main_window_remains_resizable_maximisable_and_workarea_bounded()
    print("GUI model and worker-result contract tests passed")


if __name__ == "__main__":
    main()
