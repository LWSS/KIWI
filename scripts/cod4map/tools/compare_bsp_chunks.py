#!/usr/bin/env python3
"""Compare CoD4 version-22 BSPs by tagged chunk payload.

The original writer can leak uninitialised bytes into four-byte alignment
padding.  Those bytes are not part of any chunk, so this tool reports them
separately and bases its exit status on the header and payloads only.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import struct
import sys


BSP_MAGIC = 0x50534249
BSP_VERSION = 22


@dataclasses.dataclass(frozen=True)
class Chunk:
    chunk_type: int
    payload: bytes
    padding: bytes
    offset: int


@dataclasses.dataclass(frozen=True)
class Bsp:
    path: pathlib.Path
    chunks: tuple[Chunk, ...]
    trailing: bytes


def align4(value: int) -> int:
    return (value + 3) & ~3


def read_bsp(path: pathlib.Path) -> Bsp:
    data = path.read_bytes()
    if len(data) < 12:
        raise ValueError(f"{path}: file is shorter than the 12-byte header")

    magic, version, chunk_count = struct.unpack_from("<III", data)
    if magic != BSP_MAGIC:
        raise ValueError(f"{path}: bad magic 0x{magic:08x}")
    if version != BSP_VERSION:
        raise ValueError(f"{path}: version {version}, expected {BSP_VERSION}")

    directory_end = 12 + 8 * chunk_count
    if directory_end > len(data):
        raise ValueError(f"{path}: chunk directory exceeds file size")

    entries = [struct.unpack_from("<II", data, 12 + 8 * i)
               for i in range(chunk_count)]
    cursor = directory_end
    chunks: list[Chunk] = []
    for index, (chunk_type, length) in enumerate(entries):
        payload_end = cursor + length
        padded_end = align4(payload_end)
        if padded_end > len(data):
            raise ValueError(
                f"{path}: chunk {index} type {chunk_type} exceeds file size"
            )
        chunks.append(
            Chunk(chunk_type, data[cursor:payload_end],
                  data[payload_end:padded_end], cursor)
        )
        cursor = padded_end

    return Bsp(path, tuple(chunks), data[cursor:])


def compare(left: Bsp, right: Bsp) -> bool:
    equal = True
    print(f"left : {left.path} ({left.path.stat().st_size} bytes)")
    print(f"right: {right.path} ({right.path.stat().st_size} bytes)")

    left_types = [chunk.chunk_type for chunk in left.chunks]
    right_types = [chunk.chunk_type for chunk in right.chunks]
    if left_types != right_types:
        print(f"DIFF chunk sequence: {left_types} != {right_types}")
        equal = False

        left_by_type = {chunk.chunk_type: chunk for chunk in left.chunks}
        right_by_type = {chunk.chunk_type: chunk for chunk in right.chunks}
        for chunk_type in sorted(set(left_by_type) | set(right_by_type)):
            if chunk_type not in left_by_type:
                chunk = right_by_type[chunk_type]
                print(f"DIFF type {chunk_type}: only right, length={len(chunk.payload)}")
            elif chunk_type not in right_by_type:
                chunk = left_by_type[chunk_type]
                print(f"DIFF type {chunk_type}: only left, length={len(chunk.payload)}")
            else:
                a = left_by_type[chunk_type]
                b = right_by_type[chunk_type]
                if len(a.payload) != len(b.payload):
                    print(f"DIFF type {chunk_type}: length "
                          f"{len(a.payload)} != {len(b.payload)}")
                elif a.payload != b.payload:
                    first = next(i for i, pair in enumerate(zip(a.payload, b.payload))
                                 if pair[0] != pair[1])
                    print(f"DIFF type {chunk_type}: first payload byte +0x{first:x}: "
                          f"0x{a.payload[first]:02x} != 0x{b.payload[first]:02x}")
                else:
                    print(f"PASS type {chunk_type}: length={len(a.payload)}")
        print("RESULT: semantic difference")
        return False

    for index in range(len(left.chunks)):
        a = left.chunks[index]
        b = right.chunks[index]
        prefix = f"chunk {index}"
        if a.chunk_type != b.chunk_type or len(a.payload) != len(b.payload):
            print(f"DIFF {prefix}: type/length "
                  f"{a.chunk_type}/{len(a.payload)} != "
                  f"{b.chunk_type}/{len(b.payload)}")
            equal = False
            continue

        if a.payload != b.payload:
            first = next(i for i, pair in enumerate(zip(a.payload, b.payload))
                         if pair[0] != pair[1])
            print(f"DIFF {prefix}: type={a.chunk_type} length={len(a.payload)}, "
                  f"first payload byte +0x{first:x}: "
                  f"0x{a.payload[first]:02x} != 0x{b.payload[first]:02x}")
            equal = False
        else:
            print(f"PASS {prefix}: type={a.chunk_type} length={len(a.payload)}")

        if a.padding != b.padding:
            print(f"PAD  {prefix}: {a.padding.hex() or '-'} != "
                  f"{b.padding.hex() or '-'} (ignored)")

    if left.trailing != right.trailing:
        print(f"DIFF trailing data: {len(left.trailing)} != {len(right.trailing)} "
              "bytes")
        equal = False

    print("RESULT: payload-equivalent" if equal else "RESULT: semantic difference")
    return equal


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=pathlib.Path)
    parser.add_argument("right", type=pathlib.Path)
    args = parser.parse_args()
    try:
        return 0 if compare(read_bsp(args.left), read_bsp(args.right)) else 1
    except (OSError, ValueError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
