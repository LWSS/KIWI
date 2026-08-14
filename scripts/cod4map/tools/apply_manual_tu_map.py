#!/usr/bin/env python3
"""Apply reviewed donor-to-CoD4 TU mappings and merge duplicate manifest rows."""

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


def dedupe_evidence(value: str) -> str:
    """Preserve evidence ordering while removing prior rerun duplicates."""
    parts = [part.strip() for part in value.split(";") if part.strip()]
    return "; ".join(dict.fromkeys(parts))


def main() -> None:
    args = parse_args()
    repo = args.repo.resolve()
    mapping_path = repo / "COD4MAP_MANUAL_TU_MAP.csv"
    with mapping_path.open(newline="", encoding="utf-8") as stream:
        mappings = list(csv.DictReader(stream))

    manifest_path = repo / "COD4MAP_TU_MANIFEST.csv"
    with manifest_path.open(newline="", encoding="utf-8") as stream:
        manifest = list(csv.DictReader(stream))
        manifest_columns = list(manifest[0].keys())

    # Both importers are designed for iterative use.  Keep this pass
    # idempotent too: older runs appended the same manual evidence each time.
    unique_manifest: list[dict[str, str]] = []
    seen_manifest: set[tuple[str, ...]] = set()
    for row in manifest:
        row["evidence"] = dedupe_evidence(row["evidence"])
        key = tuple(row[column] for column in manifest_columns)
        if key not in seen_manifest:
            seen_manifest.add(key)
            unique_manifest.append(row)
    manifest = unique_manifest

    remove_ids: set[int] = set()
    applied = 0
    for mapping in mappings:
        donor_row = next(
            (row for row in manifest if row["donor_tu"] == mapping["donor_tu"]),
            None,
        )
        if donor_row is None:
            raise RuntimeError(f"missing donor TU row: {mapping['donor_tu']}")
        cod4_row = next(
            (
                row
                for row in manifest
                if row is not donor_row and row["cod4_tu"] == mapping["cod4_tu"]
            ),
            None,
        )
        donor_row["cod4_tu"] = mapping["cod4_tu"]
        if cod4_row is not None:
            for field in (
                "anchor_addresses",
                "address_start",
                "address_end",
                "confidence",
                "status",
            ):
                if cod4_row[field]:
                    donor_row[field] = cod4_row[field]
            remove_ids.add(id(cod4_row))
        evidence = f"manual mapping: {mapping['evidence']}"
        if evidence not in donor_row["evidence"]:
            donor_row["evidence"] = (
                f"{donor_row['evidence']}; {evidence}"
                if donor_row["evidence"]
                else evidence
            )
        donor_row["evidence"] = dedupe_evidence(donor_row["evidence"])
        # A reviewed manual boundary mapping supersedes weak string-only
        # anchoring.  Preserve an existing EXACT result, but promote empty,
        # LOW, or MEDIUM confidence once fixed-address/object-order evidence
        # has been recorded in COD4MAP_MANUAL_TU_MAP.csv.
        if donor_row["confidence"] not in ("EXACT", "HIGH"):
            donor_row["confidence"] = "HIGH"
        donor_row["status"] = "TU_ANCHORED"
        applied += 1

    manifest = [row for row in manifest if id(row) not in remove_ids]
    manifest.sort(key=lambda row: row["cod4_tu"] or "~" + row["donor_tu"])
    with manifest_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=manifest_columns)
        writer.writeheader()
        writer.writerows(manifest)

    ledger_path = repo / "COD4MAP_FUNCTION_LEDGER.csv"
    with ledger_path.open(newline="", encoding="utf-8") as stream:
        ledger = list(csv.DictReader(stream))
        ledger_columns = list(ledger[0].keys())
    by_donor = {mapping["donor_tu"]: mapping for mapping in mappings}
    ledger_updates = 0
    for row in ledger:
        mapping = by_donor.get(row["cod4_tu"])
        if mapping is None:
            continue
        row["cod4_tu"] = mapping["cod4_tu"]
        evidence = f"reviewed TU map from {mapping['donor_tu']}"
        if evidence not in row["evidence"]:
            row["evidence"] = (
                f"{row['evidence']}; {evidence}" if row["evidence"] else evidence
            )
        ledger_updates += 1
    with ledger_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=ledger_columns)
        writer.writeheader()
        writer.writerows(ledger)

    print(f"manual TU mappings applied: {applied}")
    print(f"duplicate manifest rows merged: {len(remove_ids)}")
    print(f"existing ledger assignments canonicalized: {ledger_updates}")


if __name__ == "__main__":
    main()
