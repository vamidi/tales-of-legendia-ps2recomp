#!/usr/bin/env python3
"""Find direct dispatchGuestBranch targets that are absent from the function table.

The generated register_functions.cpp assigns a slot for every address the map
covered. The corpus dispatches to a literal address for DirectCall/DirectJump.
If that literal is not a covered slot, the runtime reports a missing target and
spins there. This lists every such gap.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "data", "output_fixed")

reg_path = os.path.join(GEN, "register_functions.cpp")
reg_re = re.compile(r"// 0x([0-9a-fA-F]+)")
registered = set()
with open(reg_path, "r", encoding="utf-8", errors="ignore") as f:
    for line in f:
        if "g_ps2RecompiledFunctionTable[" not in line:
            continue
        m = reg_re.search(line)
        if m:
            registered.add(int(m.group(1), 16))

target_re = re.compile(r"dispatchGuestBranch\(rdram, ctx, 0x([0-9a-fA-F]+)u,")
sources = {}
for path in glob.glob(os.path.join(GEN, "*.cpp")):
    if os.path.basename(path) == "register_functions.cpp":
        continue
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        text = f.read()
    for m in target_re.finditer(text):
        tgt = int(m.group(1), 16)
        sources.setdefault(tgt, set()).add(os.path.basename(path))

missing = sorted(t for t in sources if t not in registered)
print(f"registered slots: {len(registered)}")
print(f"distinct direct targets: {len(sources)}")
print(f"missing direct targets: {len(missing)}")
for t in missing:
    who = ",".join(sorted(sources[t])[:3])
    print(f"  0x{t:08X}  <- {who}")
