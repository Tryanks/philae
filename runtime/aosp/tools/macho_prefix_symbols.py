#!/usr/bin/env python3
"""Give every external symbol in a Mach-O object file the C leading underscore.

ART's assembly is written for ELF, where C symbols have no prefix: it defines
`art_quick_foo` and calls `artQuickFoo`. Mach-O C/C++ code spells those
`_art_quick_foo` and `_artQuickFoo`. Rather than touching every reference in the
assembly sources, the objects are assembled as written and their symbol tables
are rewritten here, which is what the assembler would have produced had the
sources used Mach-O names.

usage: macho_prefix_symbols.py <in.o> <out.o>
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2
N_STAB = 0xE0
N_EXT = 0x01
NLIST_64 = struct.Struct("<IBBHQ")


def main():
    src, dst = sys.argv[1:3]
    data = bytearray(open(src, "rb").read())
    magic, _cpu, _sub, _type, ncmds = struct.unpack_from("<5I", data, 0)
    if magic != MH_MAGIC_64:
        sys.exit(f"{src}: not a 64-bit Mach-O object")

    offset = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, offset)
        if cmd == LC_SYMTAB:
            break
        offset += cmdsize
    else:
        sys.exit(f"{src}: no symbol table")
    symoff, nsyms, stroff, strsize = struct.unpack_from("<4I", data, offset + 8)

    strings = bytearray(b"\0")
    for i in range(nsyms):
        at = symoff + i * NLIST_64.size
        strx, n_type, n_sect, n_desc, value = NLIST_64.unpack_from(data, at)
        end = data.index(b"\0", stroff + strx)
        name = bytes(data[stroff + strx:end])
        if n_type & N_EXT and not n_type & N_STAB:
            name = b"_" + name
        NLIST_64.pack_into(data, at, len(strings), n_type, n_sect, n_desc, value)
        strings += name + b"\0"
    strings += b"\0" * (-len(strings) % 8)

    # The string table is the last thing in an object file; replace it in place.
    if stroff + strsize != len(data):
        sys.exit(f"{src}: string table is not at the end of the file")
    del data[stroff:]
    data += strings
    struct.pack_into("<I", data, offset + 20, len(strings))
    open(dst, "wb").write(data)


if __name__ == "__main__":
    main()
