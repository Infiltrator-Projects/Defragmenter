# Defragmenter UI vision

This document records the graphical north star for the Defragmenter desktop interface.

![Defragmenter dashboard concept](ui/defragmenter-ui-vision.jpg)

## Core intent

Defragmenter should feel graphical, premium, colourful, modern, GUI-first,
welcoming and visually rich. It should not present itself like a command-line
front end, an administrative form, or an early-alpha storage utility.

The visual spirit comes from Amiga/Workbench-era desktop computing: information
is something the user can see and manipulate, not merely read. The implementation
is contemporary GTK rather than a retro imitation.

## Visual direction

- The disk map is a **continuous pixel raster**, not a visible square-cell grid.
  Every painted device pixel represents at least one whole physical storage
  unit. Left-to-right and then top-to-bottom advances monotonically through
  physical disk order. When units outnumber pixels, adjacent ranges combine
  in order; when the canvas exceeds the available detail, surplus canvas stays
  unused. No unit or analyser cell is magnified across pixels to fill space.
  The caption states the actual integer unit count or range per mapped pixel.
  Units are the native analyser's allocation units, including FAT clusters;
  clusters are never expanded into sector pixels to fill the canvas. No
  renderer may subdivide an aggregated source cell into made-up storage states.
  The renderer must never use Hilbert,
  Morton/Z-order, visible cell borders or interpolation that blurs category
  boundaries.
- The main window uses a graphical navigation rail, a selected-volume hero card,
  visual summary cards, a dominant disk-map canvas and large operation cards.
- Detailed logs remain available, but are secondary and collapsed by default.
- Common continues to own the suite typography, semantic Day/Night palette and
  neutral design metrics. Defragmenter adds only product-specific presentation
  and allocation-map category colours.
- Storage safety, operation semantics and filesystem engines remain independent
  of the presentation redesign.

## First-pass implementation

The first pass establishes the new composition without rewriting the application:
a wider dashboard canvas, graphical sidebar, hero volume selector, accented
summary cards, pixel-raster disk map, large operation cards and a compact
activity surface. Further passes can refine animation, preview states, richer
drive artwork and before/after visualisations without changing the storage
engine contract.

## Navigation contract

The left rail is **navigation**, not a second copy of the operation controls.
Overview is the graphical at-a-glance dashboard and may contain quick-action
cards. Analyse, Defragment, Growth Defrag, Recover, Test Media and Settings each
open their own section with the information and controls relevant to that task.
A sidebar click must never silently execute a destructive or long-running
operation.

## Raster-art contract

Where the approved concept uses illustration or product artwork, Defragmenter
uses dedicated standalone raster assets rather than procedurally redrawing the
scene with Cairo primitives. Runtime artwork lives under `gui/ui/art/` and is
installed into the application's private UI asset directory consumed by the
native GTK desktop; the complete concept screenshot is **documentation only**
and must never be loaded, cropped or displayed by the application. The drive
badge, hero landscape and Workbench-inspired sidebar scene each have their own
asset with no embedded prototype controls. Procedural drawing remains appropriate
for truthful live data visualisations such as gauges and the physical allocation
map.

## Primary volume-control contract

The selected-volume hero is decorative and informational. The actual volume
selector, Refresh, Open image and Unmount controls live in a separate full-width
control surface immediately below the hero. Primary controls must never be
children of the hero artwork overlay or depend on its fixed raster height.
This guarantees that the combo box and its popup remain visible, clickable and
unclipped across supported window sizes and desktop themes.

## Overview fit contract

At normal laptop work-area sizes, including a 1366×768-class desktop, the
Overview page must present its hero, volume selector, summary cards, physical
allocation map, quick actions, compact activity strip and footer without
requiring vertical scrolling. The allocation map is the elastic surface: it
grows when space is available and yields height first when the window is
shorter. The Overview must not add a second miniature allocation-map preview;
the authoritative map already occupies that role.
