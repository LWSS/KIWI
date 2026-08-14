#!/usr/bin/env python3
"""Propagate TU ownership only across spans bracketed by identical exact anchors."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[3],
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    ledger_path = args.repo.resolve() / "COD4MAP_FUNCTION_LEDGER.csv"
    with ledger_path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
        columns = list(rows[0].keys())

    exact = [
        (index, row)
        for index, row in enumerate(rows)
        if row["tu_confidence"] == "EXACT" and row["cod4_tu"]
    ]
    applied = 0
    overridden = 0
    for (left_index, left), (right_index, right) in zip(exact, exact[1:]):
        if left["cod4_tu"] != right["cod4_tu"]:
            continue
        for index in range(left_index + 1, right_index):
            row = rows[index]
            if row["tu_confidence"] == "EXACT":
                continue
            if row["cod4_tu"] and row["cod4_tu"] != left["cod4_tu"]:
                overridden += 1
            row["cod4_tu"] = left["cod4_tu"]
            row["tu_confidence"] = "HIGH"
            evidence = (
                f"bracketed by exact {left['cod4_tu']} anchors "
                f"{left['address']} and {right['address']}"
            )
            if evidence not in row["evidence"]:
                row["evidence"] = (
                    f"{row['evidence']}; {evidence}" if row["evidence"] else evidence
                )
            applied += 1

    with ledger_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)

    print(f"bracketed TU assignments applied: {applied}")
    print(f"weaker string assignments overridden: {overridden}")


if __name__ == "__main__":
    main()
