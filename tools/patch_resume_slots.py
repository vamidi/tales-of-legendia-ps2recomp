#!/usr/bin/env python3
"""Fill missing function-table slots for generated function bodies.

The runtime dispatches guest code by looking a pc up in the dense table in
`register_functions.cpp` (one slot per 4 bytes). A generated body can be
*resumed* at an interior label - a cooperative checkpoint bail sets ctx->pc to
the loop target, and the scheduler then dispatches that exact address.

The recompiler registers one slot per instruction for ordinary functions, but
it used to skip functions whose name starts with `entry_` (the "additional
entry point" continuations such as `entry_001f8158`, the GS worker loop body).
Their interior addresses therefore had a null slot: `lookupFunction()` returned
the missing-function stub, which leaves the pc unchanged, so the guest spun on
the same instruction forever (`[spin] pc=0x1F8168`).

The recompiler is fixed; this script repairs an already-generated corpus by
adding the missing slots. It only *adds* entries for addresses that are not
already registered and is idempotent.

Usage: python tools/patch_resume_slots.py [generated_dir]
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "data", "output_fixed")
REG = os.path.join(GEN, "register_functions.cpp")

ADDRESS_RE = re.compile(r"^// Address: 0x([0-9a-fA-F]+) - 0x([0-9a-fA-F]+)\s*$", re.M)
SYMBOL_RE = re.compile(r"^void\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", re.M)
SLOT_RE = re.compile(r"g_ps2RecompiledFunctionTable\[\d+\]\s*=\s*[^;]+;\s*//\s*0x([0-9a-fA-F]+)")
BASE_RE = re.compile(r"g_ps2RecompiledFunctionTableBase\s*=\s*0x([0-9a-fA-F]+)u")

with open(REG, "r", encoding="utf-8", errors="ignore") as handle:
    reg_text = handle.read()

registered = {int(m.group(1), 16) for m in SLOT_RE.finditer(reg_text)}
base_match = BASE_RE.search(reg_text)
if base_match is None:
    sys.exit("could not find g_ps2RecompiledFunctionTableBase")
table_base = int(base_match.group(1), 16)

missing = {}
for path in glob.glob(os.path.join(GEN, "*.cpp")):
    if os.path.abspath(path) == os.path.abspath(REG):
        continue
    with open(path, "r", encoding="utf-8", errors="ignore") as handle:
        text = handle.read()
    address = ADDRESS_RE.search(text)
    symbol = SYMBOL_RE.search(text)
    if address is None or symbol is None:
        continue
    start = int(address.group(1), 16)
    end = int(address.group(2), 16)
    name = symbol.group(1)
    for pc in range(start, end, 4):
        if pc not in registered and pc not in missing:
            missing[pc] = name

if not missing:
    print("no missing slots")
    sys.exit(0)

lines = []
for pc in sorted(missing):
    slot = (pc - table_base) >> 2
    lines.append(
        "        g_ps2RecompiledFunctionTable[{0}] = {1}; // 0x{2:x}".format(slot, missing[pc], pc)
    )

# Insert before the closing brace of the initializer constructor:
#     ...entries...
#     }
# };
anchor = "\n    }\n};\nstatic const GeneratedFunctionTableInitializer"
if anchor not in reg_text:
    sys.exit("could not find the initializer tail in register_functions.cpp")

patched = reg_text.replace(anchor, "\n" + "\n".join(lines) + anchor, 1)
with open(REG, "w", encoding="utf-8", newline="\n") as handle:
    handle.write(patched)

print("added {0} slot(s) for {1} distinct pc(s)".format(len(lines), len(missing)))
