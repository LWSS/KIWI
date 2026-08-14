#!/usr/bin/env python3
"""Apply conservative, globally unique CoD2 function-name anchors to IDA."""

from __future__ import annotations

import argparse
import asyncio
import collections
import csv
import json
from pathlib import Path
from typing import Any

from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client


DEFAULT_ENDPOINT = "http://127.0.0.1:13340/mcp"
ANCHOR_COLUMNS = [
    "address",
    "old_name",
    "new_name",
    "cod4_tu",
    "donor_tu",
    "score",
    "second_score",
    "unique_string_count",
    "evidence_strings",
    "apply_status",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[3],
    )
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    parser.add_argument("--apply", action="store_true")
    return parser.parse_args()


def build_candidates(repo: Path) -> list[dict[str, Any]]:
    raw_ida = json.loads(
        (repo / "IDA" / "cod4map_functions.json").read_text(encoding="utf-8")
    )
    ida_by_address = {function["address"]: function for function in raw_ida}
    ida_names = {
        function["ida_name"]: function["address"]
        for function in raw_ida
        if function["ida_name"]
    }
    donor = json.loads(
        (repo / "IDA" / "cod2_donor_functions_split.json").read_text(
            encoding="utf-8"
        )
    )
    donor_by_tu: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    global_donor_names: collections.Counter[str] = collections.Counter()
    for function in donor:
        donor_by_tu[function["donor_tu"]].append(function)
        global_donor_names[function["function"]] += 1

    with (repo / "COD4MAP_TU_MANIFEST.csv").open(
        newline="", encoding="utf-8"
    ) as stream:
        manifest = list(csv.DictReader(stream))
    donor_tu_by_cod4_tu = {
        row["cod4_tu"]: row["donor_tu"]
        for row in manifest
        if row["cod4_tu"] and row["donor_tu"]
    }
    with (repo / "COD4MAP_FUNCTION_LEDGER.csv").open(
        newline="", encoding="utf-8"
    ) as stream:
        ledger = list(csv.DictReader(stream))

    provisional: list[dict[str, Any]] = []
    for row in ledger:
        donor_tu = donor_tu_by_cod4_tu.get(row["cod4_tu"])
        ida_function = ida_by_address[row["address"]]
        if not donor_tu or not ida_function["strings"]:
            continue
        donor_functions = donor_by_tu[donor_tu]
        string_frequency = collections.Counter(
            value for function in donor_functions for value in set(function["strings"])
        )
        scored: list[tuple[float, dict[str, Any], set[str]]] = []
        for donor_function in donor_functions:
            overlap = set(ida_function["strings"]) & set(donor_function["strings"])
            overlap = {value for value in overlap if len(value.strip()) >= 4}
            if not overlap:
                continue
            score = sum(
                (1.0 + min(len(value), 120) / 80.0) / string_frequency[value]
                for value in overlap
            )
            scored.append((score, donor_function, overlap))
        scored.sort(key=lambda item: -item[0])
        if not scored:
            continue

        score, donor_function, overlap = scored[0]
        second_score = scored[1][0] if len(scored) > 1 else 0.0
        unique_count = sum(string_frequency[value] == 1 for value in overlap)
        high_confidence = (
            unique_count >= 1
            and score >= 1.4
            and score >= second_score * 1.35
        ) or (
            len(overlap) >= 3
            and score >= 2.5
            and score >= second_score * 1.5
        )
        if not high_confidence:
            continue

        new_name = donor_function["function"]
        if global_donor_names[new_name] != 1:
            continue
        existing_address = ida_names.get(new_name)
        if existing_address and existing_address != row["address"]:
            continue
        provisional.append(
            {
                "address": row["address"],
                "old_name": row["ida_name"],
                "new_name": new_name,
                "cod4_tu": row["cod4_tu"],
                "donor_tu": donor_tu,
                "score": round(score, 6),
                "second_score": round(second_score, 6),
                "unique_string_count": unique_count,
                "evidence_strings": sorted(overlap),
                "apply_status": "ALREADY_NAMED"
                if row["ida_name"] == new_name
                else "READY",
            }
        )

    by_name = collections.Counter(candidate["new_name"] for candidate in provisional)
    return [candidate for candidate in provisional if by_name[candidate["new_name"]] == 1]


async def apply_names(args: argparse.Namespace, candidates: list[dict[str, Any]]) -> None:
    pending = [candidate for candidate in candidates if candidate["apply_status"] == "READY"]
    if not pending:
        return

    async with streamablehttp_client(args.endpoint) as (read, write, _):
        async with ClientSession(read, write) as session:
            await session.initialize()
            for start in range(0, len(pending), 50):
                chunk = pending[start : start + 50]
                response = await session.call_tool(
                    "rename",
                    {
                        "batch": {
                            "func": [
                                {"addr": item["address"], "name": item["new_name"]}
                                for item in chunk
                            ],
                            "dry_run": not args.apply,
                            "allow_overwrite": False,
                            "stop_on_error": False,
                        }
                    },
                )
                if response.isError:
                    raise RuntimeError(response.content)
                for item in chunk:
                    item["apply_status"] = "APPLIED" if args.apply else "DRY_RUN_OK"


def update_ledger(repo: Path, candidates: list[dict[str, Any]]) -> None:
    applied = {
        candidate["address"]: candidate
        for candidate in candidates
        if candidate["apply_status"] in {"APPLIED", "ALREADY_NAMED"}
    }
    if not applied:
        return
    ledger_path = repo / "COD4MAP_FUNCTION_LEDGER.csv"
    with ledger_path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
        columns = list(rows[0].keys())
    for row in rows:
        candidate = applied.get(row["address"])
        if candidate is None:
            continue
        row["final_name"] = candidate["new_name"]
        row["cod2_candidate"] = candidate["new_name"]
        evidence = "name anchor from unique exact string references"
        if evidence not in row["evidence"]:
            row["evidence"] = (
                f"{row['evidence']}; {evidence}" if row["evidence"] else evidence
            )
        if row["review_status"] == "INVENTORIED":
            row["review_status"] = "NAMED_ANCHOR"
    with ledger_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def write_report(repo: Path, candidates: list[dict[str, Any]]) -> None:
    path = repo / "COD4MAP_NAME_ANCHORS.csv"
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=ANCHOR_COLUMNS)
        writer.writeheader()
        for candidate in candidates:
            row = dict(candidate)
            row["evidence_strings"] = " | ".join(candidate["evidence_strings"])
            writer.writerow(row)


def main() -> None:
    args = parse_args()
    repo = args.repo.resolve()
    candidates = build_candidates(repo)
    asyncio.run(apply_names(args, candidates))
    if args.apply:
        update_ledger(repo, candidates)
    write_report(repo, candidates)
    counts = collections.Counter(candidate["apply_status"] for candidate in candidates)
    print(f"globally unique high-confidence name anchors: {len(candidates)}")
    print(f"statuses: {dict(sorted(counts.items()))}")


if __name__ == "__main__":
    main()
