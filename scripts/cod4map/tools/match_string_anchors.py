#!/usr/bin/env python3
"""Create conservative CoD2-to-CoD4 TU anchors from exact string references."""

from __future__ import annotations

import argparse
import collections
import csv
import json
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[3],
    )
    return parser.parse_args()


def useful_string(value: str) -> bool:
    value = value.strip()
    if len(value) < 4:
        return False
    lowered = value.lower()
    if "\\msvcproj\\kiwi\\cod2map-master\\" in lowered:
        return False
    return True


def classify(
    score: float,
    second_score: float,
    hit_count: int,
    unique_hit_count: int,
) -> str:
    if (
        unique_hit_count >= 2
        and score >= 2.0
        and score >= second_score * 1.5
    ) or (
        unique_hit_count >= 1
        and hit_count >= 3
        and score >= 3.0
        and score >= second_score * 1.5
    ):
        return "HIGH"
    if (
        score >= 1.25
        and score >= second_score * 1.35
        and (unique_hit_count >= 1 or hit_count >= 2)
    ):
        return "MEDIUM"
    return "LOW"


def main() -> None:
    args = parse_args()
    repo = args.repo.resolve()
    raw_dir = repo / "IDA"
    ida_functions = json.loads(
        (raw_dir / "cod4map_functions.json").read_text(encoding="utf-8")
    )
    donor_functions = json.loads(
        (raw_dir / "cod2_donor_functions_split.json").read_text(encoding="utf-8")
    )

    ida_frequency: collections.Counter[str] = collections.Counter()
    donor_tus_by_string: dict[str, set[str]] = collections.defaultdict(set)
    donor_functions_by_string: dict[str, list[dict[str, Any]]] = (
        collections.defaultdict(list)
    )

    for function in ida_functions:
        ida_frequency.update(
            value for value in set(function["strings"]) if useful_string(value)
        )
    for function in donor_functions:
        for value in set(function["strings"]):
            if not useful_string(value):
                continue
            donor_tus_by_string[value].add(function["donor_tu"])
            donor_functions_by_string[value].append(function)

    candidates: list[dict[str, Any]] = []
    for function in ida_functions:
        tu_scores: collections.Counter[str] = collections.Counter()
        function_scores: collections.Counter[tuple[str, str]] = collections.Counter()
        hits_by_tu: dict[str, set[str]] = collections.defaultdict(set)

        for value in set(function["strings"]):
            if not useful_string(value) or value not in donor_tus_by_string:
                continue
            donor_tus = donor_tus_by_string[value]
            weight = (1.0 + min(len(value), 120) / 80.0) / (
                ida_frequency[value] * len(donor_tus)
            )
            for donor_tu in donor_tus:
                tu_scores[donor_tu] += weight
                hits_by_tu[donor_tu].add(value)
            for donor_function in donor_functions_by_string[value]:
                function_scores[
                    (donor_function["donor_tu"], donor_function["function"])
                ] += weight

        if not tu_scores:
            continue

        ranked_tus = tu_scores.most_common()
        donor_tu, score = ranked_tus[0]
        second_score = ranked_tus[1][1] if len(ranked_tus) > 1 else 0.0
        evidence_strings = sorted(
            hits_by_tu[donor_tu],
            key=lambda value: (
                ida_frequency[value] * len(donor_tus_by_string[value]),
                -len(value),
                value,
            ),
        )
        unique_hits = sum(
            1
            for value in evidence_strings
            if ida_frequency[value] == 1 and len(donor_tus_by_string[value]) == 1
        )
        confidence = classify(
            score, second_score, len(evidence_strings), unique_hits
        )

        ranked_functions = [
            {
                "donor_tu": key[0],
                "function": key[1],
                "score": round(value, 6),
            }
            for key, value in function_scores.most_common(10)
            if key[0] == donor_tu
        ]
        candidates.append(
            {
                "address": function["address"],
                "ida_name": function["ida_name"],
                "donor_tu": donor_tu,
                "confidence": confidence,
                "score": round(score, 6),
                "second_tu_score": round(second_score, 6),
                "hit_count": len(evidence_strings),
                "unique_hit_count": unique_hits,
                "candidate_functions": ranked_functions,
                "evidence_strings": evidence_strings,
            }
        )

    (raw_dir / "cod4map_string_anchor_candidates.json").write_text(
        json.dumps(candidates, indent=2) + "\n", encoding="utf-8"
    )

    manifest_path = repo / "COD4MAP_TU_MANIFEST.csv"
    with manifest_path.open(newline="", encoding="utf-8") as stream:
        manifest_rows = list(csv.DictReader(stream))
        manifest_columns = list(manifest_rows[0].keys())
    cod4_tu_by_donor_tu = {
        row["donor_tu"]: row["cod4_tu"]
        for row in manifest_rows
        if row["donor_tu"] and row["cod4_tu"]
    }

    candidates_by_address = {candidate["address"]: candidate for candidate in candidates}
    ledger_path = repo / "COD4MAP_FUNCTION_LEDGER.csv"
    with ledger_path.open(newline="", encoding="utf-8") as stream:
        ledger_rows = list(csv.DictReader(stream))
        ledger_columns = list(ledger_rows[0].keys())

    for row in ledger_rows:
        candidate = candidates_by_address.get(row["address"])
        if not candidate or candidate["confidence"] not in {"HIGH", "MEDIUM"}:
            continue
        resolved_tu = cod4_tu_by_donor_tu.get(
            candidate["donor_tu"], candidate["donor_tu"]
        )
        if not row["cod4_tu"]:
            row["cod4_tu"] = resolved_tu
            row["tu_confidence"] = candidate["confidence"]
        ranked_functions = candidate["candidate_functions"]
        if ranked_functions and not row["cod2_candidate"]:
            row["cod2_candidate"] = ranked_functions[0]["function"]
        evidence = candidate["evidence_strings"][:3]
        if evidence and not row["evidence"]:
            row["evidence"] = "exact string refs: " + " || ".join(evidence)

    with ledger_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=ledger_columns)
        writer.writeheader()
        writer.writerows(ledger_rows)

    by_tu: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for candidate in candidates:
        if candidate["confidence"] in {"HIGH", "MEDIUM"}:
            by_tu[candidate["donor_tu"]].append(candidate)

    for row in manifest_rows:
        anchors = by_tu.get(row["donor_tu"], [])
        if not anchors:
            continue
        if row["confidence"] == "EXACT":
            continue
        addresses = sorted(int(anchor["address"], 16) for anchor in anchors)
        high_count = sum(anchor["confidence"] == "HIGH" for anchor in anchors)
        medium_count = len(anchors) - high_count
        if not row["cod4_tu"]:
            row["cod4_tu"] = row["donor_tu"]
        row["anchor_addresses"] = " | ".join(hex(address) for address in addresses)
        row["confidence"] = "HIGH" if high_count else "MEDIUM"
        row["evidence"] = f"{high_count} high and {medium_count} medium string anchors"
        row["status"] = "TU_ANCHORED"

    with manifest_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=manifest_columns)
        writer.writeheader()
        writer.writerows(manifest_rows)

    counts = collections.Counter(candidate["confidence"] for candidate in candidates)
    anchored_tus = len(by_tu)
    print(f"candidate functions: {len(candidates)}")
    print(f"confidence counts: {dict(sorted(counts.items()))}")
    print(f"anchored translation units: {anchored_tus}/41")


if __name__ == "__main__":
    main()
