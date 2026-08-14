#!/usr/bin/env python3
"""Export a reproducible cod4map inventory from the live IDA MCP instance.

Raw IDA data is written below the ignored ``IDA`` directory.  The concise
function ledger is written at the repository root and is intended to be kept
under version control throughout the port.
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import hashlib
import json
from pathlib import Path
from typing import Any

from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client


DEFAULT_ENDPOINT = "http://127.0.0.1:13340/mcp"
FUNCTION_COLUMNS = [
    "address",
    "end",
    "size",
    "ida_name",
    "final_name",
    "library_status",
    "flags",
    "prototype",
    "caller_count",
    "callee_count",
    "basic_block_count",
    "string_refs",
    "cod2_candidate",
    "cod4_tu",
    "tu_confidence",
    "change_class",
    "implementation",
    "evidence",
    "test",
    "review_status",
]
GLOBAL_COLUMNS = [
    "address",
    "end",
    "size",
    "ida_name",
    "final_name",
    "segment",
    "flags",
    "prototype",
    "xref_count",
    "xref_functions",
    "cod4_tu",
    "donor_symbol",
    "change_class",
    "implementation",
    "evidence",
    "review_status",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[3],
        help="KIWI repository root",
    )
    parser.add_argument("--chunk-size", type=int, default=100)
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def function_chunk_code(offset: int, count: int, output_path: Path) -> str:
    # Executed by IDA's embedded Python.  Keep imports inside the snippet so
    # every request is independent and can be retried safely.
    return f"""
import ida_bytes
import ida_funcs
import ida_gdl
import ida_nalt
import idautils
import idc
import json

all_eas = list(idautils.Functions())
rows = []
for ea in all_eas[{offset}:{offset + count}]:
    fn = ida_funcs.get_func(ea)
    callers = set()
    callees = set()
    strings = set()

    for xref in idautils.CodeRefsTo(ea, False):
        owner = ida_funcs.get_func(xref)
        if owner is not None:
            callers.add(owner.start_ea)

    for item in idautils.FuncItems(ea):
        for target in idautils.CodeRefsFrom(item, False):
            callee = ida_funcs.get_func(target)
            if callee is not None and callee.start_ea != ea:
                callees.add(callee.start_ea)
        for target in idautils.DataRefsFrom(item):
            value = ida_bytes.get_strlit_contents(target, -1, ida_nalt.STRTYPE_C)
            if value:
                strings.add(value.decode('utf-8', errors='replace'))

    try:
        block_count = sum(1 for _ in ida_gdl.FlowChart(fn))
    except Exception:
        block_count = 0

    flags = fn.flags
    rows.append({{
        'address': hex(fn.start_ea),
        'end': hex(fn.end_ea),
        'size': fn.end_ea - fn.start_ea,
        'ida_name': ida_funcs.get_func_name(fn.start_ea),
        'flags': flags,
        'is_library': bool(flags & ida_funcs.FUNC_LIB),
        'is_thunk': bool(flags & ida_funcs.FUNC_THUNK),
        'is_noret': bool(flags & ida_funcs.FUNC_NORET),
        'prototype': idc.get_type(fn.start_ea),
        'callers': [hex(x) for x in sorted(callers)],
        'callees': [hex(x) for x in sorted(callees)],
        'basic_block_count': block_count,
        'strings': sorted(strings),
    }})
with open({str(output_path)!r}, 'w', encoding='utf-8') as stream:
    json.dump(rows, stream)
result = json.dumps({{'total': len(all_eas), 'count': len(rows)}})
"""


def global_inventory_code(output_path: Path) -> str:
    return f"""
import ida_bytes
import ida_funcs
import ida_segment
import idautils
import idc
import json

candidates = {{ea: name for ea, name in idautils.Names()}}
for fn_ea in idautils.Functions():
    for item in idautils.FuncItems(fn_ea):
        for target in idautils.DataRefsFrom(item):
            seg = ida_segment.getseg(target)
            if not seg or ida_segment.get_segm_name(seg) not in ('.rdata', '.data'):
                continue
            candidates.setdefault(
                target,
                idc.get_name(target, idc.GN_VISIBLE) or 'data_%X' % target,
            )

rows = []
for ea, name in sorted(candidates.items()):
    seg = ida_segment.getseg(ea)
    if not seg or ida_segment.get_segm_name(seg) not in ('.rdata', '.data'):
        continue
    flags = ida_bytes.get_full_flags(ea)
    if ida_bytes.is_strlit(flags):
        continue
    size = max(ida_bytes.get_item_size(ea), 1)
    owners = set()
    for xref in idautils.XrefsTo(ea, 0):
        fn = ida_funcs.get_func(xref.frm)
        if fn:
            owners.add(idc.get_func_name(fn.start_ea) or hex(fn.start_ea))
    rows.append({{
        'address': hex(ea),
        'end': hex(ea + size),
        'size': size,
        'ida_name': name,
        'segment': ida_segment.get_segm_name(seg),
        'flags': hex(flags),
        'prototype': idc.get_type(ea) or '',
        'xref_count': len(list(idautils.XrefsTo(ea, 0))),
        'xref_functions': sorted(owners),
    }})
with open({str(output_path)!r}, 'w', encoding='utf-8') as stream:
    json.dump(rows, stream)
{{'count': len(rows)}}
"""


async def call_json(session: ClientSession, tool: str, arguments: dict[str, Any]) -> Any:
    response = await session.call_tool(tool, arguments)
    if response.isError:
        raise RuntimeError(f"IDA tool {tool} failed: {response.content}")
    return response.structuredContent


async def export(args: argparse.Namespace) -> None:
    repo = args.repo.resolve()
    raw_dir = repo / "IDA"
    raw_dir.mkdir(parents=True, exist_ok=True)

    async with streamablehttp_client(args.endpoint) as (read, write, _):
        async with ClientSession(read, write) as session:
            await session.initialize()

            health = await call_json(session, "server_health", {})
            if health.get("module", "").lower() != "cod4map.exe":
                raise RuntimeError(f"wrong IDA database: {health}")
            survey = await call_json(session, "survey_binary", {"detail_level": "minimal"})

            functions: list[dict[str, Any]] = []
            offset = 0
            total: int | None = None
            chunk_path = raw_dir / "cod4map_function_chunk.json"
            while total is None or offset < total:
                payload = await call_json(
                    session,
                    "py_eval",
                    {
                        "code": function_chunk_code(
                            offset, args.chunk_size, chunk_path
                        )
                    },
                )
                if payload.get("stderr"):
                    raise RuntimeError(payload["stderr"])
                chunk = json.loads(payload["result"])
                total = int(chunk["total"])
                rows = json.loads(chunk_path.read_text(encoding="utf-8"))
                if len(rows) != int(chunk["count"]):
                    raise RuntimeError("IDA function chunk count mismatch")
                if not rows:
                    break
                functions.extend(rows)
                offset += len(rows)
                print(f"exported functions: {offset}/{total}", flush=True)

            chunk_path.unlink(missing_ok=True)

            if total is None or len(functions) != total:
                raise RuntimeError(
                    f"incomplete function export: expected {total}, got {len(functions)}"
                )

            globals_result = await call_json(
                session, "entity_query", {"queries": {"kind": "globals", "count": 0}}
            )
            imports_result = await call_json(
                session, "entity_query", {"queries": {"kind": "imports", "count": 0}}
            )
            strings_result = await call_json(
                session, "entity_query", {"queries": {"kind": "strings", "count": 0}}
            )
            global_path = raw_dir / "cod4map_global_inventory.json"
            global_payload = await call_json(
                session,
                "py_eval",
                {"code": global_inventory_code(global_path)},
            )
            if global_payload.get("stderr"):
                raise RuntimeError(global_payload["stderr"])
            global_inventory = json.loads(global_path.read_text(encoding="utf-8"))

    metadata = {
        "health": health,
        "survey": survey,
        "idb_sha256": sha256(raw_dir / "cod4map.i64"),
    }
    (raw_dir / "cod4map_metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    (raw_dir / "cod4map_functions.json").write_text(
        json.dumps(functions, indent=2) + "\n", encoding="utf-8"
    )
    (raw_dir / "cod4map_globals.json").write_text(
        json.dumps(global_inventory, indent=2) + "\n", encoding="utf-8"
    )
    (raw_dir / "cod4map_imports.json").write_text(
        json.dumps(imports_result, indent=2) + "\n", encoding="utf-8"
    )
    (raw_dir / "cod4map_strings.json").write_text(
        json.dumps(strings_result, indent=2) + "\n", encoding="utf-8"
    )

    ledger_path = repo / "COD4MAP_FUNCTION_LEDGER.csv"
    with ledger_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FUNCTION_COLUMNS)
        writer.writeheader()
        for fn in functions:
            if fn["is_library"]:
                library_status = "LIBRARY"
                change_class = "THIRD_PARTY"
            elif fn["is_thunk"]:
                library_status = "THUNK"
                change_class = "THUNK_OR_GLUE"
            else:
                library_status = "COMPILER"
                change_class = ""

            ida_name = fn["ida_name"]
            final_name = "" if ida_name.startswith("sub_") else ida_name
            writer.writerow(
                {
                    "address": fn["address"],
                    "end": fn["end"],
                    "size": fn["size"],
                    "ida_name": ida_name,
                    "final_name": final_name,
                    "library_status": library_status,
                    "flags": fn["flags"],
                    "prototype": fn["prototype"] or "",
                    "caller_count": len(fn["callers"]),
                    "callee_count": len(fn["callees"]),
                    "basic_block_count": fn["basic_block_count"],
                    "string_refs": " | ".join(fn["strings"]),
                    "change_class": change_class,
                    "review_status": "INVENTORIED",
                }
            )

    global_ledger_path = repo / "COD4MAP_GLOBAL_LEDGER.csv"
    previous_globals: dict[str, dict[str, str]] = {}
    if global_ledger_path.exists():
        with global_ledger_path.open(newline="", encoding="utf-8") as stream:
            previous_globals = {
                row["address"]: row for row in csv.DictReader(stream)
            }
    generated_columns = {
        "address",
        "end",
        "size",
        "ida_name",
        "segment",
        "flags",
        "prototype",
        "xref_count",
        "xref_functions",
    }
    with global_ledger_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=GLOBAL_COLUMNS)
        writer.writeheader()
        for item in global_inventory:
            row = {column: "" for column in GLOBAL_COLUMNS}
            row.update(previous_globals.get(item["address"], {}))
            for column in generated_columns:
                value = item[column]
                row[column] = " | ".join(value) if isinstance(value, list) else value
            if not row["final_name"] and not item["ida_name"].startswith(
                ("byte_", "word_", "dword_", "qword_", "flt_", "dbl_", "unk_", "off_")
            ):
                row["final_name"] = item["ida_name"]
            if not row["review_status"]:
                row["review_status"] = "INVENTORIED"
            writer.writerow(row)

    print(f"wrote {ledger_path}")
    print(f"wrote {global_ledger_path} ({len(global_inventory)} data items)")
    print(f"wrote raw exports under {raw_dir}")


def main() -> None:
    args = parse_args()
    if args.chunk_size <= 0:
        raise SystemExit("--chunk-size must be positive")
    asyncio.run(export(args))


if __name__ == "__main__":
    main()
