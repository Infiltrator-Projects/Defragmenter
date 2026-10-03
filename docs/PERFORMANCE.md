# Defragmenter responsiveness baseline

Defragmenter 1.8.0-240 keeps expensive presentation work out of ordinary GTK redraw and dispatch paths.

The physical allocation raster is cached by widget geometry, scale and map generation. Static hero, drive and sidebar raster artwork is likewise cached at its current widget geometry rather than bilinear-rescaled on every expose event. Cache invalidation remains tied to data, appearance or geometry changes rather than arbitrary repaint requests.

Test Media processes child stdout/stderr in bounded batches of at most 64 lines per GLib main-loop dispatch. This prevents a verbose qualification worker from monopolising the GTK thread while preserving every line and filesystem status event. Log autoscroll uses the current end iterator directly rather than allocating and deleting a temporary text mark for every line.

The native shell uses the current Infiltrator OS geometry baseline: a 44 px titlebar and 195 px navigation rail. Sidebar raster artwork is constrained within that rail rather than requesting a wider child.

The production dependency is Infiltratr Common 1.19.38 at exact commit `7070c5812b50821fd7580101cb2289a3184f6b2c`. The release build, local installer and repository submodule assert that same identity.

The retained Python GUI/reference sources are compatibility and regression material only. Shipped Defragmenter packages and the local installer use the native C/C++ desktop and exclude Python source files.
