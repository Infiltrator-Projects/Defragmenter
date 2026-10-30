# Defragmenter HFS+/HFSX SO migration
The original HFS+ native Defrag/Growth Defrag/recovery worker is now
compiled exclusively in Filesystem-HFSPlus/HFSPLUS.so. The installed
Defragmenter worker is a trusted module loader, and its regression tests
execute through the test-mode loader with independent before/after fixtures.

**Test Media:** HFS+ formatting, population and payload verification now execute inside HFSPLUS.so using the versioned format-neutral corpus host interface. The classic HFS transitional codec remains separate and is pending its own migration.
