#!/usr/bin/env python3
"""Extract function ownership and normalized signatures from donor COFF files."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any


FUNCTION_COLUMNS = [
    "donor_tu",
    "function",
    "linker_symbol",
    "storage_class",
    "section",
    "section_index",
    "offset",
    "size",
    "normalized_sha256",
    "references",
    "strings",
]

TU_COLUMNS = [
    "cod4_tu",
    "donor_tu",
    "donor_function_count",
    "donor_functions",
    "object_variant",
    "anchor_addresses",
    "address_start",
    "address_end",
    "confidence",
    "evidence",
    "status",
]


@dataclass
class Section:
    index: int
    name: str
    raw: bytes
    reloc_offset: int
    reloc_count: int


@dataclass
class Symbol:
    index: int
    name: str
    value: int
    section_index: int
    type_value: int
    storage_class: int
    aux_count: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[3],
    )
    parser.add_argument("--variant", choices=("split", "ordered"), default="split")
    return parser.parse_args()


def read_c_string(data: bytes, offset: int, limit: int = 2048) -> str:
    if offset < 0 or offset >= len(data):
        return ""
    end = data.find(b"\0", offset, min(len(data), offset + limit))
    if end < 0:
        end = min(len(data), offset + limit)
    return data[offset:end].decode("utf-8", errors="replace")


def decode_name(raw: bytes, string_table: bytes) -> str:
    if raw[:4] == b"\0\0\0\0":
        offset = struct.unpack_from("<I", raw, 4)[0]
        return read_c_string(string_table, offset)
    return raw.rstrip(b"\0").decode("ascii", errors="replace")


def clean_linker_name(name: str) -> str:
    if name.startswith("_") and not name.startswith("__"):
        name = name[1:]
    if "@" in name and name.rsplit("@", 1)[-1].isdigit():
        name = name.rsplit("@", 1)[0]
    return name


def looks_like_string(value: bytes) -> bool:
    if len(value) < 2:
        return False
    printable = sum(byte in (9, 10, 13) or 32 <= byte < 127 for byte in value)
    return printable / len(value) >= 0.90


def parse_object(path: Path, donor_tu: str) -> list[dict[str, Any]]:
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"truncated COFF object: {path}")

    machine, section_count, _, symbol_offset, symbol_count, optional_size, _ = (
        struct.unpack_from("<HHIIIHH", data, 0)
    )
    if machine != 0x14C:
        raise ValueError(f"expected x86 COFF object, got machine {machine:#x}: {path}")

    string_offset = symbol_offset + symbol_count * 18
    if string_offset + 4 > len(data):
        raise ValueError(f"invalid COFF string table: {path}")
    string_size = struct.unpack_from("<I", data, string_offset)[0]
    string_table = data[string_offset : string_offset + string_size]

    sections: dict[int, Section] = {}
    section_header_offset = 20 + optional_size
    for section_index in range(1, section_count + 1):
        offset = section_header_offset + (section_index - 1) * 40
        raw_name = data[offset : offset + 8]
        name = raw_name.rstrip(b"\0").decode("ascii", errors="replace")
        if name.startswith("/") and name[1:].isdigit():
            name = read_c_string(string_table, int(name[1:]))
        (
            _,
            _,
            raw_size,
            raw_offset,
            reloc_offset,
            _,
            reloc_count,
            _,
            _,
        ) = struct.unpack_from("<IIIIIIHHI", data, offset + 8)
        sections[section_index] = Section(
            index=section_index,
            name=name,
            raw=data[raw_offset : raw_offset + raw_size] if raw_offset else b"",
            reloc_offset=reloc_offset,
            reloc_count=reloc_count,
        )

    symbols: list[Symbol | None] = [None] * symbol_count
    index = 0
    while index < symbol_count:
        offset = symbol_offset + index * 18
        raw_name = data[offset : offset + 8]
        value, section_index, type_value, storage_class, aux_count = struct.unpack_from(
            "<IhHBB", data, offset + 8
        )
        symbols[index] = Symbol(
            index=index,
            name=decode_name(raw_name, string_table),
            value=value,
            section_index=section_index,
            type_value=type_value,
            storage_class=storage_class,
            aux_count=aux_count,
        )
        index += 1 + aux_count

    relocs_by_section: dict[int, list[tuple[int, Symbol, int]]] = {}
    for section_index, section in sections.items():
        relocs: list[tuple[int, Symbol, int]] = []
        for reloc_index in range(section.reloc_count):
            offset = section.reloc_offset + reloc_index * 10
            virtual_address, symbol_index, reloc_type = struct.unpack_from(
                "<IIH", data, offset
            )
            symbol = symbols[symbol_index]
            if symbol is not None:
                relocs.append((virtual_address, symbol, reloc_type))
        relocs_by_section[section_index] = relocs

    function_symbols = [
        symbol
        for symbol in symbols
        if symbol is not None
        and symbol.section_index > 0
        and symbol.section_index in sections
        and sections[symbol.section_index].name.startswith(".text")
        and symbol.type_value & 0x20
        and not symbol.name.startswith("$")
    ]

    functions: list[dict[str, Any]] = []
    for symbol in function_symbols:
        section = sections[symbol.section_index]
        same_section = sorted(
            (
                other
                for other in function_symbols
                if other.section_index == symbol.section_index
                and other.value > symbol.value
            ),
            key=lambda item: item.value,
        )
        end = same_section[0].value if same_section else len(section.raw)
        code = bytearray(section.raw[symbol.value:end])
        references: set[str] = set()
        strings: set[str] = set()

        for reloc_offset, target, reloc_type in relocs_by_section[symbol.section_index]:
            if not symbol.value <= reloc_offset < end:
                continue
            references.add(clean_linker_name(target.name))
            local_offset = reloc_offset - symbol.value
            width = 2 if reloc_type == 0x000A else 4
            for byte_index in range(local_offset, min(local_offset + width, len(code))):
                code[byte_index] = 0

            target_section = sections.get(target.section_index)
            if target_section is not None and target_section.name.startswith(".rdata"):
                raw_string = target_section.raw[target.value :]
                terminator = raw_string.find(b"\0")
                if terminator >= 0:
                    raw_string = raw_string[:terminator]
                if looks_like_string(raw_string):
                    strings.add(raw_string.decode("utf-8", errors="replace"))

        functions.append(
            {
                "donor_tu": donor_tu,
                "function": clean_linker_name(symbol.name),
                "linker_symbol": symbol.name,
                "storage_class": symbol.storage_class,
                "section": section.name,
                "section_index": symbol.section_index,
                "offset": symbol.value,
                "size": end - symbol.value,
                "normalized_sha256": hashlib.sha256(code).hexdigest(),
                "references": sorted(references),
                "strings": sorted(strings),
            }
        )

    functions.sort(key=lambda fn: (fn["section_index"], fn["offset"], fn["function"]))
    return functions


def main() -> None:
    args = parse_args()
    repo = args.repo.resolve()
    object_root = repo / "build" / "cod4map-obj-split" / f"donor-{args.variant}"
    objects = sorted(object_root.glob("*.obj"))
    if len(objects) != 41:
        raise SystemExit(f"expected 41 objects under {object_root}, found {len(objects)}")

    all_functions: list[dict[str, Any]] = []
    tu_rows: list[dict[str, Any]] = []
    for object_path in objects:
        donor_tu = object_path.stem.replace("__", "\\") + ".c"
        functions = parse_object(object_path, donor_tu)
        all_functions.extend(functions)
        tu_rows.append(
            {
                "cod4_tu": "",
                "donor_tu": donor_tu,
                "donor_function_count": len(functions),
                "donor_functions": " | ".join(fn["function"] for fn in functions),
                "object_variant": args.variant,
                "anchor_addresses": "",
                "address_start": "",
                "address_end": "",
                "confidence": "",
                "evidence": "",
                "status": "DONOR_INVENTORIED",
            }
        )

    raw_dir = repo / "IDA"
    raw_dir.mkdir(parents=True, exist_ok=True)
    (raw_dir / f"cod2_donor_functions_{args.variant}.json").write_text(
        json.dumps(all_functions, indent=2) + "\n", encoding="utf-8"
    )

    with (repo / "COD4MAP_DONOR_FUNCTIONS.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=FUNCTION_COLUMNS)
        writer.writeheader()
        for function in all_functions:
            row = dict(function)
            row["references"] = " | ".join(function["references"])
            row["strings"] = " | ".join(function["strings"])
            writer.writerow(row)

    with (repo / "COD4MAP_TU_MANIFEST.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=TU_COLUMNS)
        writer.writeheader()
        writer.writerows(tu_rows)

    print(f"objects: {len(objects)}")
    print(f"donor functions: {len(all_functions)}")
    print(f"translation units: {len(tu_rows)}")


if __name__ == "__main__":
    main()
