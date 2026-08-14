#!/usr/bin/env python3
"""Apply reviewed global-data names and ledger dispositions."""

from __future__ import annotations

import argparse
import asyncio
import csv
from pathlib import Path

from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client


DEFAULT_ENDPOINT = "http://127.0.0.1:13340/mcp"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    parser.add_argument("--apply-names", action="store_true")
    return parser.parse_args()


async def rename_globals(endpoint: str, rows: list[dict[str, str]], ledger: dict[str, dict[str, str]]) -> None:
    operations = []
    for row in rows:
        current = ledger[row["address"]]["ida_name"]
        if current != row["final_name"]:
            operations.append({"old": current, "new": row["final_name"]})
    if not operations:
        return
    async with streamablehttp_client(endpoint) as (read, write, _):
        async with ClientSession(read, write) as session:
            await session.initialize()
            result = await session.call_tool("rename", {"batch": {"data": operations}})
            if result.isError:
                raise RuntimeError(f"IDA data rename failed: {result.content}")


def main() -> None:
    args = parse_args()
    repo = args.repo.resolve()
    with (repo / "COD4MAP_MANUAL_GLOBAL_MAP.csv").open(newline="", encoding="utf-8") as stream:
        overrides = list(csv.DictReader(stream))

    ledger_path = repo / "COD4MAP_GLOBAL_LEDGER.csv"
    with ledger_path.open(newline="", encoding="utf-8") as stream:
        ledger_rows = list(csv.DictReader(stream))
        columns = list(ledger_rows[0].keys())
    ledger = {row["address"]: row for row in ledger_rows}

    missing = [row["address"] for row in overrides if row["address"] not in ledger]
    if missing:
        raise RuntimeError(f"global overrides absent from inventory: {missing}")
    if args.apply_names:
        asyncio.run(rename_globals(args.endpoint, overrides, ledger))

    for override in overrides:
        row = ledger[override["address"]]
        for field in ("final_name", "prototype", "cod4_tu", "change_class", "implementation"):
            source = "type" if field == "prototype" else field
            if override[source]:
                row[field] = override[source]
        evidence = f"manual review: {override['evidence']}"
        if evidence not in row["evidence"]:
            row["evidence"] = f"{row['evidence']}; {evidence}" if row["evidence"] else evidence
        row["review_status"] = "MANUALLY_REVIEWED"

    with ledger_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(ledger_rows)
    print(f"manual global overrides applied: {len(overrides)}")


if __name__ == "__main__":
    main()
