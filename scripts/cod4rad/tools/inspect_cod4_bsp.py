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
LIGHTGRID_ENTRIES = 2
LIGHTGRID_COLORS = 3
LIGHTGRID_HEADER = 44
LIGHTGRID_ROWS = 45
LIGHTGRID_COLOR_STRIDE = 168
LIGHTGRID_ENTRY_STRIDE = 4


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


def inspect_lightgrid(bsp: Bsp, data: bytes) -> dict:
    """Decode and cross-check the four CoD4 v22 light-grid lumps."""
    errors: list[str] = []
    warnings: list[str] = []
    grouped: dict[int, list[Lump]] = defaultdict(list)
    for lump in bsp.lumps:
        grouped[lump.type].append(lump)

    def payload(lump_type: int) -> bytes:
        matches = grouped.get(lump_type, [])
        if not matches:
            errors.append(f"missing lump {lump_type}")
            return b""
        if len(matches) != 1:
            errors.append(f"lump {lump_type} occurs {len(matches)} times (expected once)")
        lump = matches[0]
        return data[lump.offset:lump.offset + lump.length]

    entries_data = payload(LIGHTGRID_ENTRIES)
    colors_data = payload(LIGHTGRID_COLORS)
    header_data = payload(LIGHTGRID_HEADER)
    rows_data = payload(LIGHTGRID_ROWS)
    sizes = {
        "entries": len(entries_data), "colors": len(colors_data),
        "header": len(header_data), "rows": len(rows_data),
    }

    if len(entries_data) % LIGHTGRID_ENTRY_STRIDE:
        errors.append(f"lump 2 size {len(entries_data)} is not divisible by 4")
    if len(colors_data) % LIGHTGRID_COLOR_STRIDE:
        errors.append(f"lump 3 size {len(colors_data)} is not divisible by 168")
    entry_count = len(entries_data) // LIGHTGRID_ENTRY_STRIDE
    color_count = len(colors_data) // LIGHTGRID_COLOR_STRIDE
    if not entry_count:
        errors.append("light-grid entry table is empty")
    if not color_count:
        errors.append("light-grid color table is empty")

    entry_info = {
        "count": entry_count, "stride": LIGHTGRID_ENTRY_STRIDE,
        "min_color_index": None, "max_color_index": None,
        "nonzero_color_indices": 0, "nonzero_needs_trace": 0,
    }
    if entry_count:
        color_indices = [struct.unpack_from("<H", entries_data, i * 4)[0]
                         for i in range(entry_count)]
        entry_info.update({
            "min_color_index": min(color_indices),
            "max_color_index": max(color_indices),
            "nonzero_color_indices": sum(index != 0 for index in color_indices),
            "nonzero_needs_trace": sum(entries_data[i * 4 + 3] != 0
                                       for i in range(entry_count)),
        })
        invalid = sorted({index for index in color_indices if index >= color_count})
        if invalid:
            shown = ", ".join(str(index) for index in invalid[:8])
            suffix = "..." if len(invalid) > 8 else ""
            errors.append(
                f"{len(invalid)} color index value(s) exceed color count {color_count}: {shown}{suffix}"
            )

    header_info: dict[str, object] = {}
    row_info: dict[str, object] = {
        "nonempty_rows": 0, "empty_rows": 0, "encoded_columns": 0,
        "skipped_columns": 0, "referenced_entries": 0,
    }
    intervals: list[tuple[int, int, int]] = []
    if len(header_data) < 20:
        errors.append(f"lump 44 is {len(header_data)} bytes (minimum header is 20)")
    else:
        mins_maxs_axes = struct.unpack_from("<6HII", header_data)
        mins = list(mins_maxs_axes[0:3])
        maxs = list(mins_maxs_axes[3:6])
        row_axis, col_axis = mins_maxs_axes[6:8]
        header_info.update({"mins": mins, "maxs": maxs,
                            "row_axis": row_axis, "col_axis": col_axis})

        for axis in range(3):
            if mins[axis] > maxs[axis]:
                errors.append(f"header min[{axis}] {mins[axis]} exceeds max {maxs[axis]}")
        if row_axis not in (0, 1) or col_axis not in (0, 1) or row_axis == col_axis:
            errors.append(f"invalid row/column axes {row_axis}/{col_axis}")
        else:
            row_count = maxs[row_axis] - mins[row_axis] + 1
            expected_header_size = 20 + row_count * 2
            header_info["row_count"] = row_count
            header_info["expected_size"] = expected_header_size
            header_info["world_mins"] = [
                (mins[0] - 0x1000) * 32,
                (mins[1] - 0x1000) * 32,
                (mins[2] - 0x0800) * 64,
            ]
            header_info["world_maxs"] = [
                (maxs[0] - 0x1000) * 32,
                (maxs[1] - 0x1000) * 32,
                (maxs[2] - 0x0800) * 64,
            ]
            if len(header_data) != expected_header_size:
                errors.append(
                    f"header size {len(header_data)} does not equal 20 + 2*{row_count} = {expected_header_size}"
                )
            else:
                row_offsets = list(struct.unpack_from(f"<{row_count}H", header_data, 20))
                nonempty = [(index, offset * 4) for index, offset in enumerate(row_offsets)
                            if offset != 0xFFFF]
                row_info["nonempty_rows"] = len(nonempty)
                row_info["empty_rows"] = row_count - len(nonempty)
                if not nonempty:
                    errors.append("header contains no non-empty row offsets")
                else:
                    if row_offsets[0] == 0xFFFF or row_offsets[-1] == 0xFFFF:
                        errors.append("first or last row in header bounds is empty")
                    physical_offsets = [offset for _, offset in nonempty]
                    if len(set(physical_offsets)) != len(physical_offsets):
                        errors.append("two light-grid rows share the same row-data offset")

                    ordered = sorted(nonempty, key=lambda item: item[1])
                    for physical_index, (header_row, row_start) in enumerate(ordered):
                        row_end = (ordered[physical_index + 1][1]
                                   if physical_index + 1 < len(ordered) else len(rows_data))
                        row_coord = mins[row_axis] + header_row
                        if row_start % 4:
                            errors.append(f"row {row_coord} starts at unaligned byte {row_start}")
                        if row_start + 12 > row_end or row_end > len(rows_data):
                            errors.append(
                                f"row {row_coord} range [{row_start}, {row_end}) cannot hold its 12-byte header"
                            )
                            continue

                        col_start, col_count, z_start, z_count, first_entry = struct.unpack_from(
                            "<4HI", rows_data, row_start
                        )
                        if not col_count or not z_count:
                            errors.append(f"row {row_coord} has zero column or z count")
                            continue
                        if col_start < mins[col_axis] or col_start + col_count - 1 > maxs[col_axis]:
                            errors.append(f"row {row_coord} column range is outside header bounds")
                        if z_start < mins[2] or z_start + z_count - 1 > maxs[2]:
                            errors.append(f"row {row_coord} z range is outside header bounds")

                        cursor = row_start + 12
                        columns = 0
                        referenced = 0
                        encoded_columns = 0
                        skipped_columns = 0
                        while columns < col_count:
                            if cursor + 2 > row_end:
                                errors.append(f"row {row_coord} record stream ends early")
                                break
                            run_count = rows_data[cursor]
                            run_height = rows_data[cursor + 1]
                            if not run_count:
                                errors.append(f"row {row_coord} contains a zero-length record")
                                break
                            if run_height == 0:
                                cursor += 2
                                skipped_columns += run_count
                            else:
                                record_size = 4 if z_count > 255 else 3
                                if cursor + record_size > row_end:
                                    errors.append(f"row {row_coord} run record ends early")
                                    break
                                z_offset = rows_data[cursor + 2]
                                if record_size == 4:
                                    z_offset |= rows_data[cursor + 3] << 8
                                if z_offset + run_height > z_count:
                                    errors.append(f"row {row_coord} run exceeds its z range")
                                cursor += record_size
                                encoded_columns += run_count
                                referenced += run_count * run_height
                            columns += run_count
                            if columns > col_count:
                                errors.append(f"row {row_coord} records exceed declared column count")
                                break

                        padding = rows_data[cursor:row_end]
                        if len(padding) > 3 or any(padding):
                            errors.append(f"row {row_coord} has invalid trailing padding")
                        if first_entry + referenced > entry_count:
                            errors.append(f"row {row_coord} references past the entry table")
                        intervals.append((first_entry, first_entry + referenced, row_coord))
                        row_info["encoded_columns"] += encoded_columns
                        row_info["skipped_columns"] += skipped_columns
                        row_info["referenced_entries"] += referenced

    expected_entry = 0
    for begin, end, row_coord in sorted(intervals):
        if begin != expected_entry:
            errors.append(
                f"row {row_coord} begins at entry {begin}; contiguous stream expected {expected_entry}"
            )
        if end < begin:
            errors.append(f"row {row_coord} has an inverted entry interval")
        expected_entry = max(expected_entry, end)
    if intervals and expected_entry != entry_count:
        errors.append(
            f"row streams reference {expected_entry} entries but lump 2 contains {entry_count}"
        )
    if rows_data and not intervals:
        warnings.append("row lump is non-empty but no row stream could be decoded")

    return {
        "valid": not errors,
        "lump_sizes": sizes,
        "header": header_info,
        "rows": row_info,
        "entries": entry_info,
        "colors": {"count": color_count, "stride": LIGHTGRID_COLOR_STRIDE},
        "errors": errors,
        "warnings": warnings,
    }


def print_lightgrid(report: dict) -> None:
    status = "VALID" if report["valid"] else "INVALID"
    sizes = report["lump_sizes"]
    print(f"lightgrid: {status}; lump bytes "
          f"2={sizes['entries']} 3={sizes['colors']} 44={sizes['header']} 45={sizes['rows']}")
    header = report["header"]
    if header:
        print(f"  header mins={header.get('mins')} maxs={header.get('maxs')} "
              f"axes={header.get('row_axis')}/{header.get('col_axis')} "
              f"world={header.get('world_mins')}..{header.get('world_maxs')}")
    rows = report["rows"]
    print(f"  rows nonempty={rows['nonempty_rows']} empty={rows['empty_rows']} "
          f"encoded-cols={rows['encoded_columns']} skipped-cols={rows['skipped_columns']} "
          f"entry-refs={rows['referenced_entries']}")
    entries = report["entries"]
    colors = report["colors"]
    print(f"  entries count={entries['count']} color-index="
          f"{entries['min_color_index']}..{entries['max_color_index']} "
          f"needs-trace={entries['nonzero_needs_trace']}; colors count={colors['count']}")
    for message in report["warnings"]:
        print(f"  warning: {message}")
    for message in report["errors"]:
        print(f"  error: {message}")


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
    parser.add_argument("--lightgrid", action="store_true",
                        help="decode and validate v22 light-grid lumps 2/3/44/45")
    parser.add_argument("--json", action="store_true", help="emit machine-readable output")
    args = parser.parse_args()

    try:
        left, left_data = parse(args.bsp)
        right_parsed = parse(args.other) if args.other else None
        right = right_parsed[0] if right_parsed else None
        lightgrid = inspect_lightgrid(left, left_data) if args.lightgrid else None
        right_lightgrid = (inspect_lightgrid(right, right_parsed[1])
                           if args.lightgrid and right and right_parsed else None)
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
            if lightgrid is not None:
                result["lightgrid"] = lightgrid
            if right:
                result["other"] = summarize(right)
                result["comparison"] = compare(left, right)
                if right_lightgrid is not None:
                    result["other_lightgrid"] = right_lightgrid
            print(json.dumps(result, indent=2))
        else:
            print_listing(left)
            if lightgrid is not None:
                print_lightgrid(lightgrid)
            if extracted:
                for dest in extracted:
                    print(f"extracted {dest}")
            if right:
                result = compare(left, right)
                print(f"compare: {'identical' if result['identical_bytes'] else 'different'}; "
                      f"{len(result['changed_types'])} changed type(s), size delta {result['size_delta']:+d}")
                for item in result["changed_types"]:
                    print(f"  type {item['type']}: {len(item['left'])} -> {len(item['right'])} chunk(s)")
                if right_lightgrid is not None:
                    print_lightgrid(right_lightgrid)
        if lightgrid is not None and not lightgrid["valid"]:
            return 1
        if right_lightgrid is not None and not right_lightgrid["valid"]:
            return 1
        return 0
    except (OSError, ValueError) as exc:
        print(f"inspect_cod4_bsp: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
