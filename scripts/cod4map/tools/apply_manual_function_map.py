#!/usr/bin/env python3
"""Apply reviewed per-function corrections after automated matching passes."""

from __future__ import annotations

import argparse
import asyncio
import csv
from pathlib import Path

from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client


DEFAULT_ENDPOINT = "http://127.0.0.1:13340/mcp"


# The early native object split used a few source paths which are represented
# by consolidated KIWI translation units.  Keep the reviewed native evidence
# in the manual map, but write the path which actually owns the implementation
# into the derived ledger.
COD4MAP_TU_ALIASES = {
    "common\\brush_edges.cpp": "brush_edges.cpp",
    "universal\\com_vector.cpp": "universal\\com_math.cpp",
    "portals_write.cpp": "portals.cpp",
    "tris_tree.cpp": "tree.cpp",
    "tris_combinelayers.cpp": "tris.cpp",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    parser.add_argument("--apply-names", action="store_true")
    return parser.parse_args()


async def rename_functions(endpoint: str, rows: list[dict[str, str]]) -> None:
    pending = [row for row in rows if row["final_name"]]
    if not pending:
        return
    async with streamablehttp_client(endpoint) as (read, write, _):
        async with ClientSession(read, write) as session:
            await session.initialize()
            result = await session.call_tool(
                "rename",
                {
                    "batch": {
                        "func": [
                            {"addr": row["address"], "name": row["final_name"]}
                            for row in pending
                        ]
                    }
                },
            )
            if result.isError:
                raise RuntimeError(f"IDA rename failed: {result.content}")


def main() -> None:
    args = parse_args()
    repo = args.repo.resolve()
    with (repo / "COD4MAP_MANUAL_FUNCTION_MAP.csv").open(
        newline="", encoding="utf-8"
    ) as stream:
        overrides = list(csv.DictReader(stream))
    overrides_by_address = {row["address"].lower(): row for row in overrides}

    ledger_path = repo / "COD4MAP_FUNCTION_LEDGER.csv"
    with ledger_path.open(newline="", encoding="utf-8") as stream:
        ledger = list(csv.DictReader(stream))
        columns = list(ledger[0].keys())

    applied = 0
    for row in ledger:
        override = overrides_by_address.get(row["address"].lower())
        if override is None:
            continue
        for field in (
            "final_name",
            "tu_confidence",
            "library_status",
            "change_class",
        ):
            if override[field]:
                row[field] = override[field]
        if override["cod4_tu"]:
            row["cod4_tu"] = COD4MAP_TU_ALIASES.get(
                override["cod4_tu"], override["cod4_tu"]
            )
        evidence = f"manual review: {override['evidence']}"
        if evidence not in row["evidence"]:
            row["evidence"] = f"{row['evidence']}; {evidence}" if row["evidence"] else evidence
        row["review_status"] = "MANUALLY_REVIEWED"
        implementation = repo / "src" / "cod4map" / row["cod4_tu"].replace("\\", "/")
        if implementation.is_file():
            row["implementation"] = implementation.relative_to(repo).as_posix()
        if override["final_name"] == "PrintBSPUsage":
            row["test"] = "startup usage oracle: exact 32-line match"
        elif override["final_name"] == "ParseBSPOption":
            row["test"] = "unknown-option oracle: exact 37-line match and exit -1"
        applied += 1

    with ledger_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(ledger)

    if args.apply_names:
        asyncio.run(rename_functions(args.endpoint, overrides))
    print(f"manual function overrides applied: {applied}")


if __name__ == "__main__":
    main()
