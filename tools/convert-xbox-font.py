#!/usr/bin/env python3
"""Convert a v1 Xbox 360 XTT font to a local TrueType file."""

import argparse
from pathlib import Path
import struct
import zlib


def checksum(data):
    padded = data + bytes(-len(data) % 4)
    return sum(word for (word,) in struct.iter_unpack(">I", padded)) & 0xffffffff


def convert(source):
    if len(source) < 0x118 or source[:4] != b"xttf":
        raise ValueError("not an XTT font")
    _, size, packed_size, table_size, version = struct.unpack_from(">5I", source, 0x104)
    if size != len(source) or version != 0x10000:
        raise ValueError("unsupported XTT container")
    directory = zlib.decompress(source[0x118:0x118 + packed_size])
    if len(directory) != table_size or directory[:4] != b"\0\1\0\0":
        raise ValueError("invalid XTT tables")

    tables = {}
    pages = []
    count, = struct.unpack_from(">H", directory, 4)
    for i in range(count):
        tag, _, offset, length = struct.unpack_from(">4sIII", directory, 12 + 16*i)
        if tag == b"xglf":
            if length % 4096 or offset + length > len(source):
                raise ValueError("invalid XTT glyph pages")
            pages = [zlib.decompress(source[p:p + 4096])
                     for p in range(offset, offset + length, 4096)]
        elif tag in (b"cmap", b"head", b"hhea", b"hmtx", b"name", b"xloc"):
            if offset + length > len(directory):
                raise ValueError("truncated XTT table")
            tables[tag] = directory[offset:offset + length]

    starts = [0]
    for page in pages:
        starts.append(starts[-1] + len(page))
    offsets = []
    for (address,) in struct.iter_unpack(">I", tables.pop(b"xloc")):
        page, offset = divmod(address, 65536)
        if page >= len(pages) or offset > len(pages[page]):
            raise ValueError("invalid XTT glyph offset")
        offsets.append(starts[page] + offset)
    if offsets != sorted(offsets) or len(offsets) < 2:
        raise ValueError("invalid XTT glyph ordering")

    tables[b"glyf"] = b"".join(pages)[:offsets[-1]]
    tables[b"loca"] = struct.pack(f">{len(offsets)}I", *offsets)
    head = bytearray(tables[b"head"])
    struct.pack_into(">I", head, 8, 0)
    struct.pack_into(">h", head, 50, 1)  # Long loca offsets.
    tables[b"head"] = bytes(head)
    tables[b"maxp"] = struct.pack(">IH13H", 0x10000, len(offsets) - 1,
                                  4096, 512, 4096, 512, 2, 0, 0, 0,
                                  0, 0, 0, 64, 16)
    tables[b"post"] = struct.pack(">I28x", 0x30000)
    count = len(tables)
    power = count.bit_length() - 1
    search_range = 16 << power
    result = bytearray(struct.pack(">I4H", 0x10000, count, search_range,
                                   power, 16 * count - search_range))
    offset = 12 + 16 * count
    head_offset = None
    payload = bytearray()
    for tag, data in sorted(tables.items()):
        if tag == b"head":
            head_offset = offset
        result.extend(struct.pack(">4s3I", tag, checksum(data), offset, len(data)))
        payload.extend(data)
        payload.extend(bytes(-len(data) % 4))
        offset = 12 + 16 * count + len(payload)
    result.extend(payload)
    struct.pack_into(">I", result, head_offset + 8, (0xB1B0AFBA - checksum(result)) & 0xffffffff)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_bytes(convert(args.source.read_bytes()))
