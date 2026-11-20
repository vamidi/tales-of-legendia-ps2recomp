#!/usr/bin/env python3
"""Persistent PCSX2 status poller: samples the guest PC in a tight loop over one socket and prints a PC
histogram in the same shape as the port's [pchist] line, so the two can be diffed at a matched milestone.

python tools/pc_hist_ref.py [seconds] [top]
"""
import json
import socket
import sys
import time
from collections import Counter

HOST, PORT = "127.0.0.1", 21512


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    topn = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    s = socket.create_connection((HOST, PORT), timeout=10.0)
    s.settimeout(10.0)
    f = s.makefile("rwb")
    hist = Counter()
    t0 = time.time()
    n = 0
    while time.time() - t0 < seconds:
        f.write((json.dumps({"cmd": "status"}) + "\n").encode())
        f.flush()
        line = f.readline()
        if not line:
            break
        try:
            r = json.loads(line.decode("utf-8", "replace"))
        except Exception:
            continue
        d = r.get("data") or {}
        pc = d.get("pc")
        if isinstance(pc, str):
            hist[pc.upper().replace("0X", "")] += 1
            n += 1
    s.close()
    print("[pcref] n=%d distinct=%d over %.1fs" % (n, len(hist), time.time() - t0))
    for pc, c in hist.most_common(topn):
        print("  %s=%d" % (pc, c))


if __name__ == "__main__":
    main()
