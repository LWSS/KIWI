#!/usr/bin/env python3
"""Apply source-file references embedded in cod4map as TU ownership anchors.

An embedded ``.cpp`` string is strong evidence, but it is not automatically the
owner of the function containing it: an inlined assertion can carry the source
path of the callee into a different translation unit.  References are therefore
clustered by address.  A TU's dominant cluster is eligible as ownership
evidence, while separated outliers are retained in the report only.
"""

from __future__ import annotations

import argparse
import csv
import json
import ntpath
import re
from pathlib import Path


ANCHOR_COLUMNS = [
    "address",
    "ida_name",
    "selected_tu",
    "candidate_tus",
    "anchor_class",
    "raw_paths",
]

# Function sections from one compiler TU can be spread out, but the observed
# source-path anchors within real cod4map's main clusters are all much closer
# than this.  A larger separation is characteristic of an inlined path ref.
CLUSTER_GAP = 0x10000

# These assignments were checked at their fixed native addresses.  They are
# intentional source-path overrides rather than broad filename heuristics:
# the KIWI implementation was split differently from the native CoD4 object
# layout, so its physical source file is not evidence of native TU ownership.
REVIEWED_CONFLICT_OVERRIDES = {
    "0x4031f0": "brush.cpp",
    "0x4032a0": "brush.cpp",
    "0x412400": "common\\collvec.cpp",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[3],
    )
    return parser.parse_args()


def equivalent_tu(left: str, right: str) -> bool:
    def key(value: str) -> str:
        value = value.lower().replace("/", "\\")
        return re.sub(r"\.(?:c|cpp)$", "", value)

    if not left or not right:
        return False
    if key(left) == key(right):
        return True
    left_stem = Path(left.replace("\\", "/")).stem.lower()
    right_stem = Path(right.replace("\\", "/")).stem.lower()
    return left_stem == right_stem


def canonicalize_source_path(value: str) -> str | None:
    original = value.strip().replace("/", "\\")
    lowered = original.lower()
    if not re.search(r"\.(?:c|cpp)$", lowered):
        return None

    # Statically linked MSVC runtime paths are useful for runtime
    # classification, but are not cod4map translation units.
    if "vctools\\crt_bld" in lowered or re.match(
        r"^(?:_file|strerror|onexit|output|stream|input|read|tidtable|mbctype|mlock|winsig|inithelp|setlocal|_sftbuf|ioinit|drive|tzset|gmtime|stdenvp|stdargv|a_env|_getbuf|osfinfo|inittime|initnum|initmon|initctyp|strftime|convrtcp|x10fout|wtombenv|setenv)\.c$",
        lowered,
    ):
        return None
    if lowered.startswith("i386\\"):
        return None

    lowered = lowered.removeprefix(".\\")
    lowered = lowered.removeprefix("..\\")
    lowered = lowered.replace("src\\universal\\..\\", "")
    lowered = lowered.replace("src\\", "")

    cod3_marker = "cod3src\\"
    if cod3_marker in lowered:
        lowered = lowered.split(cod3_marker, 1)[1]
        lowered = lowered.removeprefix("src\\")

    # One string begins one byte into "common" in the original binary.
    if lowered.startswith("ommon\\"):
        lowered = "c" + lowered

    lowered = lowered.replace("physics\\ode\\src\\", "physics\\ode\\")
    lowered = ntpath.normpath(lowered)
    return lowered


def main() -> None:
    args = parse_args()
    repo = args.repo.resolve()
    raw_functions = json.loads(
        (repo / "IDA" / "cod4map_functions.json").read_text(encoding="utf-8")
    )

    anchors: list[dict[str, str]] = []
    for function in raw_functions:
        raw_paths = sorted(
            {
                value.strip()
                for value in function["strings"]
                if re.search(r"(?i)\.(?:c|cpp|h)$", value.strip())
            }
        )
        candidate_tus = sorted(
            {
                candidate
                for candidate in (canonicalize_source_path(value) for value in raw_paths)
                if candidate
            }
        )
        if not candidate_tus:
            continue
        selected = candidate_tus[0] if len(candidate_tus) == 1 else ""
        anchors.append(
            {
                "address": function["address"],
                "ida_name": function["ida_name"],
                "selected_tu": selected,
                "candidate_tus": " | ".join(candidate_tus),
                "anchor_class": "",
                "raw_paths": " | ".join(raw_paths),
            }
        )

    # Select the largest coherent address cluster for each unambiguous TU.
    # This keeps inlined source paths visible without treating them as owners.
    anchors_by_tu: dict[str, list[dict[str, str]]] = {}
    for anchor in anchors:
        if anchor["selected_tu"]:
            anchors_by_tu.setdefault(anchor["selected_tu"], []).append(anchor)

    selected_by_address: dict[str, tuple[str, list[str], str]] = {}
    for tu, tu_anchors in anchors_by_tu.items():
        ordered = sorted(tu_anchors, key=lambda row: int(row["address"], 16))
        clusters: list[list[dict[str, str]]] = []
        for anchor in ordered:
            if (
                not clusters
                or int(anchor["address"], 16)
                - int(clusters[-1][-1]["address"], 16)
                > CLUSTER_GAP
            ):
                clusters.append([])
            clusters[-1].append(anchor)
        dominant = max(
            clusters,
            key=lambda cluster: (
                len(cluster),
                -int(cluster[0]["address"], 16),
            ),
        )
        dominant_addresses = {row["address"] for row in dominant}
        for anchor in ordered:
            if anchor["address"] not in dominant_addresses:
                anchor["anchor_class"] = "INLINE_PATH_OUTLIER"
                continue
            anchor_class = (
                "DOMINANT_SOURCE_CLUSTER"
                if len(dominant) > 1
                else "SINGLE_SOURCE_REF"
            )
            anchor["anchor_class"] = anchor_class
            selected_by_address[anchor["address"]] = (
                tu,
                anchor["raw_paths"].split(" | "),
                anchor_class,
            )

    anchor_path = repo / "COD4MAP_SOURCE_PATH_ANCHORS.csv"
    with anchor_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=ANCHOR_COLUMNS)
        writer.writeheader()
        writer.writerows(anchors)

    ledger_path = repo / "COD4MAP_FUNCTION_LEDGER.csv"
    with ledger_path.open(newline="", encoding="utf-8") as stream:
        ledger_rows = list(csv.DictReader(stream))
        ledger_columns = list(ledger_rows[0].keys())

    applied = 0
    conflicts: list[tuple[str, str, str]] = []
    reviewed_overrides = 0
    for row in ledger_rows:
        selected_data = selected_by_address.get(row["address"])
        if selected_data is None:
            continue
        selected, raw_paths, anchor_class = selected_data
        existing_tu = row["cod4_tu"]
        if existing_tu and not equivalent_tu(existing_tu, selected):
            reviewed_tu = REVIEWED_CONFLICT_OVERRIDES.get(row["address"].lower())
            if reviewed_tu is None or not equivalent_tu(reviewed_tu, selected):
                conflicts.append((row["address"], existing_tu, selected))
                continue
            reviewed_overrides += 1
        row["cod4_tu"] = selected
        row["tu_confidence"] = (
            "EXACT" if anchor_class == "DOMINANT_SOURCE_CLUSTER" else "HIGH"
        )
        evidence = f"{anchor_class.lower()}: {raw_paths[0]}"
        if row["address"].lower() in REVIEWED_CONFLICT_OVERRIDES:
            evidence = f"reviewed native source-path override: {raw_paths[0]}"
        if evidence not in row["evidence"]:
            row["evidence"] = (
                f"{row['evidence']}; {evidence}" if row["evidence"] else evidence
            )
        applied += 1

    with ledger_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=ledger_columns)
        writer.writeheader()
        writer.writerows(ledger_rows)

    manifest_path = repo / "COD4MAP_TU_MANIFEST.csv"
    with manifest_path.open(newline="", encoding="utf-8") as stream:
        manifest_rows = list(csv.DictReader(stream))
        manifest_columns = list(manifest_rows[0].keys())

    # This pass is intentionally safe to rerun.  Older revisions only marked
    # donor-backed rows as represented, so every rerun appended another copy
    # of each CoD4-only TU.  Collapse byte-for-byte duplicate rows before
    # applying anchors and account for existing CoD4-only rows below.
    unique_manifest_rows: list[dict[str, str]] = []
    seen_manifest_rows: set[tuple[str, ...]] = set()
    for row in manifest_rows:
        key = tuple(row[column] for column in manifest_columns)
        if key in seen_manifest_rows:
            continue
        seen_manifest_rows.add(key)
        unique_manifest_rows.append(row)
    manifest_rows = unique_manifest_rows

    exact_by_tu: dict[str, list[str]] = {}
    anchor_class_by_tu: dict[str, str] = {}
    for address, (selected, _, anchor_class) in selected_by_address.items():
        exact_by_tu.setdefault(selected, []).append(address)
        anchor_class_by_tu[selected] = anchor_class

    represented: set[str] = {
        row["cod4_tu"]
        for row in manifest_rows
        if row["cod4_tu"] in exact_by_tu
    }
    for row in manifest_rows:
        matching_candidates = [
            tu
            for tu in exact_by_tu
            if equivalent_tu(row["donor_tu"], tu)
        ]
        if not matching_candidates and row["donor_tu"]:
            donor_stem = Path(row["donor_tu"].replace("\\", "/")).stem.lower()
            matching_candidates = [
                tu
                for tu in exact_by_tu
                if Path(tu.replace("\\", "/")).stem.lower() == donor_stem
            ]
        matching_tu = matching_candidates[0] if len(matching_candidates) == 1 else None
        if matching_tu is None:
            continue
        represented.add(matching_tu)
        addresses = sorted(exact_by_tu[matching_tu], key=lambda value: int(value, 16))
        row["cod4_tu"] = matching_tu
        row["anchor_addresses"] = " | ".join(addresses)
        row["confidence"] = (
            "EXACT"
            if anchor_class_by_tu[matching_tu] == "DOMINANT_SOURCE_CLUSTER"
            else "HIGH"
        )
        row["evidence"] = f"{len(addresses)} ownership-eligible source-path anchors"
        row["status"] = "TU_ANCHORED"

    for cod4_tu, addresses in sorted(exact_by_tu.items()):
        if cod4_tu in represented:
            continue
        manifest_rows.append(
            {
                "cod4_tu": cod4_tu,
                "donor_tu": "",
                "donor_function_count": "",
                "donor_functions": "",
                "object_variant": "",
                "anchor_addresses": " | ".join(
                    sorted(addresses, key=lambda value: int(value, 16))
                ),
                "address_start": "",
                "address_end": "",
                "confidence": (
                    "EXACT"
                    if anchor_class_by_tu[cod4_tu] == "DOMINANT_SOURCE_CLUSTER"
                    else "HIGH"
                ),
                "evidence": f"{len(addresses)} ownership-eligible source-path anchors",
                "status": "COD4_TU_ANCHORED",
            }
        )

    manifest_rows.sort(key=lambda row: (row["cod4_tu"] or "~" + row["donor_tu"]))
    with manifest_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=manifest_columns)
        writer.writeheader()
        writer.writerows(manifest_rows)

    print(f"source-path anchor rows: {len(anchors)}")
    outliers = sum(
        anchor["anchor_class"] == "INLINE_PATH_OUTLIER" for anchor in anchors
    )
    print(f"ownership-eligible anchors applied: {applied}")
    print(f"inline path outliers excluded: {outliers}")
    print(f"reviewed source-path conflicts applied: {reviewed_overrides}")
    print(f"conflicts left for manual review: {len(conflicts)}")
    for address, existing_tu, selected_tu in conflicts:
        print(f"  {address}: {existing_tu} -> {selected_tu}")
    print(f"exactly anchored TUs: {len(exact_by_tu)}")


if __name__ == "__main__":
    main()
