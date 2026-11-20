#!/usr/bin/env python3
"""Extract one logical partition from an Android emulator system.img.

The emulator's system.img is a GPT disk whose "super" partition holds Android
dynamic partitions (liblp metadata). macOS has no tool that understands that
layout, and only the geometry, partition and extent tables are needed here.

usage: lpunpack.py <system.img> <partition-name> <output>
"""
import struct
import sys

SECTOR = 512
LP_GEOMETRY_MAGIC = 0x616C4467
LP_HEADER_MAGIC = 0x414C5030
LP_RESERVED = 4096
LP_GEOMETRY_SIZE = 4096
LP_TARGET_LINEAR = 0


def find_super(disk):
    """Byte offset of the GPT partition named 'super'."""
    disk.seek(SECTOR)
    header = disk.read(92)
    if header[:8] != b"EFI PART":
        return 0  # a bare super image
    entries_lba, count, entry_size = struct.unpack_from("<QII", header, 72)
    disk.seek(entries_lba * SECTOR)
    for _ in range(count):
        entry = disk.read(entry_size)
        first_lba = struct.unpack_from("<Q", entry, 32)[0]
        name = entry[56:128].decode("utf-16-le").rstrip("\0")
        if name == "super":
            return first_lba * SECTOR
    sys.exit("no 'super' partition in the GPT")


def main():
    image, wanted, output = sys.argv[1:4]
    with open(image, "rb") as disk:
        base = find_super(disk)
        disk.seek(base + LP_RESERVED)
        magic, _size = struct.unpack("<II", disk.read(8))
        if magic != LP_GEOMETRY_MAGIC:
            sys.exit("super partition has no liblp geometry")

        # Primary metadata follows the two geometry copies.
        meta = base + LP_RESERVED + 2 * LP_GEOMETRY_SIZE
        disk.seek(meta)
        header = disk.read(256)
        magic, _major, _minor, header_size = struct.unpack_from("<IHHI", header, 0)
        if magic != LP_HEADER_MAGIC:
            sys.exit("bad liblp metadata header")
        # header: magic, versions, header_size, header_checksum[32], tables_size,
        # tables_checksum[32], then four {offset, num_entries, entry_size} descriptors.
        descriptors = struct.unpack_from("<12I", header, 12 + 32 + 4 + 32)
        (p_off, p_num, p_size), (e_off, e_num, e_size) = descriptors[0:3], descriptors[3:6]
        tables = meta + header_size

        def table(offset, count, size):
            disk.seek(tables + offset)
            return [disk.read(size) for _ in range(count)]

        extents = [struct.unpack_from("<QIQI", e) for e in table(e_off, e_num, e_size)]
        names = []
        for entry in table(p_off, p_num, p_size):
            name = entry[:36].rstrip(b"\0").decode()
            _attrs, first, count, _group = struct.unpack_from("<4I", entry, 36)
            names.append(name)
            if name != wanted or count == 0:
                continue
            with open(output, "wb") as out:
                for sectors, target_type, target_data, _source in extents[first:first + count]:
                    if target_type != LP_TARGET_LINEAR:
                        out.write(b"\0" * (sectors * SECTOR))
                        continue
                    disk.seek(base + target_data * SECTOR)
                    remaining = sectors * SECTOR
                    while remaining:
                        chunk = disk.read(min(remaining, 1 << 24))
                        if not chunk:
                            sys.exit("extent runs past the end of the image")
                        out.write(chunk)
                        remaining -= len(chunk)
            return
    sys.exit(f"partition {wanted!r} not found; image has: {', '.join(names)}")


if __name__ == "__main__":
    main()
