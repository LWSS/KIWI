#!/usr/bin/env python3
"""List, extract, or compare Call of Duty BSP lumps without modifying them.

CoD BSP versions 19 and newer use a compact chunk table: ``IBSP, version,
chunkCount``, followed by ``{type, length}`` records.  Chunk bytes follow the
table sequentially and each chunk is rounded up to a four-byte boundary.
Versions 6 through 18 retain the older fixed {offset, length} directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path


IDENT = b"IBSP"
LEGACY_LUMP_COUNTS = (41, 41, 42, 43, 43, 43, 43, 44, 44, 44, 46, 46, 47)


@dataclass(frozen=True)
class Lump:
    type: int
    occurrence: int
    offset: int
    length: int
    sha256: str


@dataclass
class Bsp:
    path: Path
    version: int
    layout: str
    size: int
    sha256: str
    lumps: list[Lump]


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _need(data: bytes, end: int, what: str) -> None:
    if end > len(data):
        raise ValueError(f"{what} extends past end of file ({end:#x} > {len(data):#x})")


def parse(path: Path) -> tuple[Bsp, bytes]:
    data = path.read_bytes()
    _need(data, 12, "BSP header")
    ident, version, count = struct.unpack_from("<4sII", data)
    if ident != IDENT:
        shown = ident.decode("latin1", errors="replace")
        raise ValueError(f"bad BSP magic {shown!r}; expected 'IBSP'")

    entries: list[tuple[int, int, int]] = []
    if version >= 19:
        if version > 22:
            raise ValueError(f"unsupported tagged BSP version {version} (expected 19..22)")
        table_end = 12 + count * 8
        _need(data, table_end, f"tagged chunk table ({count} entries)")
        offset = table_end
        for index in range(count):
            lump_type, length = struct.unpack_from("<II", data, 12 + index * 8)
            end = offset + length
            _need(data, end, f"chunk {index} (type {lump_type})")
            entries.append((lump_type, offset, length))
            offset = (end + 3) & ~3
            # The engine only needs the aligned offset when another chunk
            # follows; it does not require trailing padding after the last.
            if index + 1 < count:
                _need(data, offset, f"padding after chunk {index}")
        layout = "tagged"
    elif 6 <= version <= 18:
        # Legacy BspHeader is {ident, version, lump[fixed-count]}, where each
        # directory item is {offset, length}.  The third header dword is the
        # first directory offset, not a tagged-chunk count.
        count = LEGACY_LUMP_COUNTS[version - 6]
        table_end = 8 + count * 8
        _need(data, table_end, f"legacy lump directory ({count} entries)")
        for lump_type in range(count):
            offset, length = struct.unpack_from("<II", data, 8 + lump_type * 8)
            _need(data, offset + length, f"legacy lump {lump_type}")
            entries.append((lump_type, offset, length))
        layout = "legacy"
    else:
        raise ValueError(f"unsupported BSP version {version} (expected 6..22)")

    occurrences: dict[int, int] = defaultdict(int)
    lumps: list[Lump] = []
    for lump_type, offset, length in entries:
        occurrence = occurrences[lump_type]
        occurrences[lump_type] += 1
        lumps.append(Lump(lump_type, occurrence, offset, length, _digest(data[offset:offset + length])))
    return Bsp(path.resolve(), version, layout, len(data), _digest(data), lumps), data


def summarize(bsp: Bsp) -> dict:
    return {
        "path": str(bsp.path), "version": bsp.version, "layout": bsp.layout,
        "size": bsp.size, "sha256": bsp.sha256, "lumps": [asdict(x) for x in bsp.lumps],
    }


def compare(left: Bsp, right: Bsp) -> dict:
    grouped: dict[int, list[Lump]] = defaultdict(list)
    for lump in left.lumps:
        grouped[lump.type].append(lump)
    right_grouped: dict[int, list[Lump]] = defaultdict(list)
    for lump in right.lumps:
        right_grouped[lump.type].append(lump)

    changed = []
    for lump_type in sorted(set(grouped) | set(right_grouped)):
        a = [(x.length, x.sha256) for x in grouped[lump_type]]
        b = [(x.length, x.sha256) for x in right_grouped[lump_type]]
        if a != b:
            changed.append({"type": lump_type, "left": a, "right": b})
    return {
        "identical_bytes": left.sha256 == right.sha256,
        "left_size": left.size, "right_size": right.size,
        "size_delta": right.size - left.size,
        "changed_types": changed,
    }


def parse_type(value: str) -> int:
    try:
        result = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("lump type must be an integer (for example 1 or 0x1)") from exc
    if not 0 <= result <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("lump type must fit an unsigned 32-bit value")
    return result


def print_listing(bsp: Bsp) -> None:
    print(f"{bsp.path}: IBSP v{bsp.version}, {bsp.layout}, {bsp.size} bytes")
    print(f"file sha256 {bsp.sha256}")
    print(" type  #      offset      length  sha256")
    for lump in bsp.lumps:
        print(f"{lump.type:5d} {lump.occurrence:2d}  {lump.offset:#10x} {lump.length:11d}  {lump.sha256}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bsp", type=Path, help="input .d3dbsp/.bsp file")
    parser.add_argument("other", nargs="?", type=Path, help="optional BSP to compare by lump type")
    parser.add_argument("--extract", type=parse_type, metavar="TYPE", help="write every matching lump")
    parser.add_argument("--out-dir", type=Path, default=Path("."), help="destination for --extract (default: current directory)")
    parser.add_argument("--json", action="store_true", help="emit machine-readable output")
    args = parser.parse_args()

    try:
        left, left_data = parse(args.bsp)
        right = parse(args.other)[0] if args.other else None
        extracted: list[str] = []
        if args.extract is not None:
            selected = [x for x in left.lumps if x.type == args.extract]
            if not selected:
                raise ValueError(f"no lump of type {args.extract} in {left.path}")
            args.out_dir.mkdir(parents=True, exist_ok=True)
            for lump in selected:
                dest = args.out_dir / f"{left.path.stem}.lump{lump.type}.{lump.occurrence}.bin"
                dest.write_bytes(left_data[lump.offset:lump.offset + lump.length])
                extracted.append(str(dest.resolve()))
        if args.json:
            result = {"bsp": summarize(left), "extracted": extracted}
            if right:
                result["other"] = summarize(right)
                result["comparison"] = compare(left, right)
            print(json.dumps(result, indent=2))
        else:
            print_listing(left)
            if extracted:
                for dest in extracted:
                    print(f"extracted {dest}")
            if right:
                result = compare(left, right)
                print(f"compare: {'identical' if result['identical_bytes'] else 'different'}; "
                      f"{len(result['changed_types'])} changed type(s), size delta {result['size_delta']:+d}")
                for item in result["changed_types"]:
                    print(f"  type {item['type']}: {len(item['left'])} -> {len(item['right'])} chunk(s)")
        return 0
    except (OSError, ValueError) as exc:
        print(f"inspect_cod4_bsp: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
