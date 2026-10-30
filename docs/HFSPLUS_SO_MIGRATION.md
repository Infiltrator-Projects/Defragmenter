# Defragmenter HFS+/HFSX SO migration
The original HFS+ native Defrag/Growth Defrag/recovery worker is now
compiled exclusively in Filesystem-HFSPlus/HFSPLUS.so. The installed
Defragmenter worker is a trusted module loader, and its regression tests
execute through the test-mode loader with independent before/after fixtures.

**Incomplete**: the Test Media HFS+/HFSX formatter and raw verifier in
`test_media_raw_mac.c` still link the legacy native library. The core
library remains temporarily only for these paths. It must be moved to
HFSPLUS.so before the overall migration can be described as complete.
