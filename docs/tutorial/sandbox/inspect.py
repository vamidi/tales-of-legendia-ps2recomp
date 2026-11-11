#!/usr/bin/env python3
"""inspect.py -- read a PS2 ELF the way the R5900 sees it.

Tutorial sandbox tool for "Part 1: the PS2 ecosystem".
Standard library only. Read-only: it never writes to the ELF.

Usage:
    python inspect.py <elf> --header
    python inspect.py <elf> --segments
    python inspect.py <elf> --va 0x00394378 --dump 0x40
"""
import struct
import sys


def load(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise SystemExit("not an ELF: " + path)
    if data[4] != 1:
        raise SystemExit("not a 32-bit ELF (EI_CLASS=%d)" % data[4])

    e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags = \
        struct.unpack_from("<HHIIIII", data, 0x10)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x2A)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x2E)

    phdrs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from("<IIIIIIII", data, off)
        phdrs.append(dict(type=p_type, offset=p_offset, vaddr=p_vaddr,
                          filesz=p_filesz, memsz=p_memsz, flags=p_flags,
                          align=p_align))

    shdrs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = \
            struct.unpack_from("<IIIIII", data, off)
        shdrs.append(dict(name=sh_name, type=sh_type, addr=sh_addr,
                          offset=sh_offset, size=sh_size))

    return data, dict(entry=e_entry, machine=e_machine, flags=e_flags,
                      type=e_type, phdrs=phdrs, shdrs=shdrs)


def resolve(elf, va):
    """Map a guest virtual address to a file offset, or say why we cannot."""
    for ph in elf["phdrs"]:
        if ph["type"] != 1:  # PT_LOAD
            continue
        if ph["vaddr"] <= va < ph["vaddr"] + ph["memsz"]:
            delta = va - ph["vaddr"]
            if delta < ph["filesz"]:
                return ph["offset"] + delta, ph
            return None, ph           # inside the segment, past EOF: .bss
    return None, None


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    data, elf = load(argv[1])
    what = argv[2]

    if what == "--header":
        print("file             %s (%d bytes)" % (argv[1], len(data)))
        print("e_type           %d%s" % (elf["type"], " (EXEC)" if elf["type"] == 2 else ""))
        print("e_machine        0x%04X%s" % (elf["machine"],
              " (MIPS, EE R5900)" if elf["machine"] == 8 else ""))
        print("e_flags          0x%08X" % elf["flags"])
        print("e_entry          0x%08X" % elf["entry"])
        print("program headers  %d" % len(elf["phdrs"]))
        print("section headers  %d" % len(elf["shdrs"]))
        loads = [p for p in elf["phdrs"] if p["type"] == 1]
        if loads:
            lo = min(p["vaddr"] for p in loads)
            hi = max(p["vaddr"] + p["memsz"] for p in loads)
            print("load span        0x%08X .. 0x%08X  (%d bytes)" % (lo, hi, hi - lo))
        return

    if what == "--segments":
        print("%-6s %-10s %-10s %-10s %-10s %-5s" %
              ("TYPE", "OFFSET", "VADDR", "FILESZ", "MEMSZ", "FLAGS"))
        for ph in elf["phdrs"]:
            if ph["type"] != 1:
                continue
            f = "".join(c if ph["flags"] & b else "-"
                        for c, b in (("r", 4), ("w", 2), ("x", 1)))
            print("%-6s 0x%08X 0x%08X 0x%08X 0x%08X %-5s" %
                  ("LOAD", ph["offset"], ph["vaddr"], ph["filesz"],
                   ph["memsz"], f))
        return

    if what == "--va":
        if len(argv) < 4:
            raise SystemExit("--va needs an address")
        va = int(argv[3], 16)
        n = 0x40
        if len(argv) >= 6 and argv[4] == "--dump":
            n = int(argv[5], 16)
        off, ph = resolve(elf, va)
        if ph is None:
            print("0x%08X: not inside any PT_LOAD segment" % va)
            print("         it is I/O, scratchpad, BIOS or simply unmapped")
            return
        if off is None:
            print("0x%08X: inside PT_LOAD 0x%08X-0x%08X but past p_filesz "
                  "(zero-initialised .bss; not stored in the file)"
                  % (va, ph["vaddr"], ph["vaddr"] + ph["memsz"]))
            return
        print("0x%08X -> file offset 0x%08X" % (va, off))
        blob = data[off:off + n]
        for i in range(0, len(blob), 16):
            row = blob[i:i + 16]
            print("%08X  %-47s  %s" %
                  (va + i, row.hex(" "), "".join(chr(c) if 32 <= c < 127 else "."
                                                 for c in row)))
        return

    raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
