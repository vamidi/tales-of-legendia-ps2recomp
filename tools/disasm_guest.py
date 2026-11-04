#!/usr/bin/env python3
"""Disassemble a virtual-address range of a PS2 ELF with capstone.

Usage:
    python tools/disasm_guest.py data/CD/SLUS_212.01 0x1AD5A0 0x1AD728
    python tools/disasm_guest.py <elf> <start> --count 80

Ghidra does not disassemble regions that are only reached through a function
pointer table (the CRI device vtable at 0x003A6A98 is one), so this helper is
the reliable way to read those bodies.
"""
import struct
import sys

from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS64, CS_MODE_LITTLE_ENDIAN


def load_segments(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise SystemExit("not an ELF: " + path)
    is64 = data[4] == 2
    e_phoff = struct.unpack_from("<I", data, 0x1C)[0] if not is64 else struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x2A)[0]
    e_phnum = struct.unpack_from("<H", data, 0x2C)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if is64:
            p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQ", data, off)
        else:
            p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIIII", data, off)
        if p_type == 1:  # PT_LOAD
            segs.append((p_vaddr, p_offset, p_filesz))
    return data, segs


def va_to_offset(segs, va):
    for vaddr, offset, filesz in segs:
        if vaddr <= va < vaddr + filesz:
            return offset + (va - vaddr)
    return None


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    elf = argv[1]
    start = int(argv[2], 16)
    if len(argv) > 3 and argv[3].startswith("0x"):
        end = int(argv[3], 16)
        size = end - start
    else:
        size = 0
        i = 3
        while i < len(argv) - 1:
            if argv[i] == "--count":
                size = int(argv[i + 1]) * 4
            i += 1
        if size == 0:
            size = 0x100

    data, segs = load_segments(elf)
    off = va_to_offset(segs, start)
    if off is None:
        raise SystemExit("0x%08X is not in a PT_LOAD segment" % start)
    blob = data[off:off + size]

    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS64 + CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    for ins in md.disasm(blob, start):
        print("%08X  %-24s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))


if __name__ == "__main__":
    main(sys.argv)
