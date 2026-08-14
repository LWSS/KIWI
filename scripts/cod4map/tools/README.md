# cod4map analysis tools

Run the inventory pipeline from the KIWI repository root with the x86
`cod4map.exe` IDA MCP endpoint listening on `127.0.0.1:13340`:

1. `export_ida_inventory.py`
2. `extract_donor_objects.py`
3. `apply_source_path_anchors.py`
4. `apply_manual_tu_map.py`
5. `match_string_anchors.py`
6. `propagate_tu_anchors.py`
7. `apply_name_anchors.py --apply`
8. `apply_manual_function_map.py`
9. `apply_manual_global_map.py`

Use `compare_bsp_chunks.py oracle.d3dbsp candidate.d3dbsp` for version-22
differential tests.  It compares the tagged directory and every payload while
reporting four-byte alignment padding separately; the original writer can
leave nondeterministic stack bytes in padding.

The exporter intentionally regenerates IDA-derived columns. Run every later
step in order so TU, manual-review, and implementation evidence is restored.
Use `--apply-names` on the manual mapping tools only when new reviewed names
must be written into IDA, then save the IDB and rerun the full pipeline.
