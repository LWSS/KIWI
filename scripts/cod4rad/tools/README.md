# CoD4Rad inventory tools

With the x86 `cod4rad.exe` IDA MCP server listening on port 13338, run:

1. `python scripts/cod4rad/tools/export_ida_inventory.py`
2. `python scripts/cod4rad/tools/apply_source_path_anchors.py`

The exporter writes raw `IDA/cod4rad_*.json` snapshots and preserves manual
columns in the two root-level ledgers.  The second pass uses embedded native
source paths only as conservative TU evidence.

`compare_rad_outputs.py` requires `--game-root`; it links the six game-data
directories into disposable roots while `maps/` remains an isolated real
directory. The links disappear with the temporary roots. With `--keep`, only
the writable retail/candidate `maps/` outputs are retained; game-data junctions
are deliberately not copied.

`compare_rad_outputs.py` stages a BSP and adjacent `.d3dprt`/`.d3dpoly` files
into separate temporary game roots before running retail and candidate tools.

`inspect_cod4_bsp.py` is a read-only BSP lister/differ.  It follows CoD4's
v19+ tagged chunk layout (sequential, four-byte aligned payloads), with a
legacy v6..18 fixed-directory fallback.  For example:

`python scripts/cod4rad/tools/inspect_cod4_bsp.py retail.d3dbsp candidate.d3dbsp`

Use `--extract 1 --out-dir extracted` to save a matching lump, or `--json` for
a complete machine-readable lump/hash report.
