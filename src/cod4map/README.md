# KIWI cod4map

This directory is the native x86 CoD4 map-compiler port.  Its first-party
baseline was imported from the GPL CoD2 reconstruction and mapped onto the
CoD4 translation-unit names recovered from `cod4map.exe`.  The baseline is not
considered verified merely because it builds: function-by-function status is
tracked in `COD4MAP_FUNCTION_LEDGER.csv` at the repository root.

The target deliberately uses KIWI's zlib 1.1.4 sources and a standalone copy
of KIWI's CoD4-era unzip implementation.  The latter is kept local because the
game version routes archive I/O back through the engine filesystem, while the
compiler owns a standalone filesystem.
