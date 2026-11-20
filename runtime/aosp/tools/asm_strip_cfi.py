#!/usr/bin/env python3
"""Copy an ART assembly source with its .cfi_* directives blanked out.

ART's unwind annotations rely on ELF conventions: `.L` assembler-local labels and
global labels in the middle of a procedure. Mach-O's assembler treats `.L` labels
as real symbols and rejects both inside a .cfi_startproc/.cfi_endproc pair. The
annotations only serve native unwinders (ART walks managed stacks itself), so the
Darwin build drops them. Lines are blanked, not removed, to keep line numbers.

usage: asm_strip_cfi.py <in.S> <out.S>
"""
import re
import sys

src, dst = sys.argv[1:3]
cfi = re.compile(r"^\s*\.cfi_\w+\b.*$")
with open(src) as f, open(dst, "w") as out:
    for line in f:
        out.write("\n" if cfi.match(line) else line)
