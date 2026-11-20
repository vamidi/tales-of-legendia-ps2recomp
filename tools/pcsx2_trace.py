#!/usr/bin/env python3
"""Trace guest breakpoints in PCSX2 and log call arguments.

Usage:
  python pcsx2_trace.py <seconds> <addr[:cond]> [addr...]

Example:
  python pcsx2_trace.py 120 0x00204240 0x00285E80 0x002853D0

Each stop logs: hit#, pc, a0..a3, ra, sp, plus a few fields that matter for the
Tales of Legendia loader/scene (loader object at a0: +0x14 mode, +0x18 arg,
+0x20 overlay; scene object at a0: +0x08 entity count). Resumes automatically.
"""
import json
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 21512


class Dbg:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
        self.s.settimeout(10)
        self.buf = b""

    def cmd(self, req):
        self.s.sendall((json.dumps(req) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.s.recv(1 << 16)
            if not chunk:
                raise RuntimeError("debug server closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode())

    def ok(self, req):
        r = self.cmd(req)
        if not r.get("ok"):
            raise RuntimeError(f"{req.get('cmd')}: {r.get('error')}")
        return r

    def status(self):
        return self.ok({"cmd": "status", "cpu": "ee"}).get("data", {})

    def gprs(self):
        d = self.ok({"cmd": "read_registers", "cpu": "ee", "category": 0}).get("data", {})
        out = {}
        for r in d.get("GPR", {}).get("regs", []):
            out[r["name"]] = int(r["value"][-8:], 16)
        return out

    def u32(self, addr):
        r = self.ok({"cmd": "read_memory", "cpu": "ee", "address": addr, "length": 4})
        return int.from_bytes(bytes.fromhex(r.get("hex", "00000000")), "little")

    def f32(self, addr):
        import struct
        return struct.unpack("<f", self.u32(addr).to_bytes(4, "little"))[0]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    seconds = float(sys.argv[1])
    specs = sys.argv[2:]
    d = Dbg()
    for i, spec in enumerate(specs, 1):
        addr, _, cond = spec.partition(":")
        req = {"cmd": "set_breakpoint", "cpu": "ee", "address": int(addr, 0), "temporary": False}
        if cond:
            req["condition"] = cond
        d.ok(req)
    print(f"breakpoints: {specs}  window {seconds}s", flush=True)
    d.ok({"cmd": "resume", "cpu": "ee"})
    t0 = time.time()
    hits = 0
    last_pc = None
    try:
        while time.time() - t0 < seconds:
            st = d.status()
            if not st.get("paused"):
                time.sleep(0.15)
                continue
            g = d.gprs()
            pc = g.get("pc", 0)
            hits += 1
            line = (f"#{hits:4d} t={time.time()-t0:7.2f}s pc=0x{pc:08X} "
                    f"a0=0x{g.get('a0',0):08X} a1=0x{g.get('a1',0):08X} "
                    f"a2=0x{g.get('a2',0):08X} v0=0x{g.get('v0',0):08X} ra=0x{g.get('ra',0):08X} sp=0x{g.get('sp',0):08X}")
            a0 = g.get("a0", 0)
            if 0x00100000 <= a0 < 0x02000000:
                if pc == 0x00204240:
                    line += (f"  mode(+14)=0x{d.u32(a0+0x14):08X} arg(+18)=0x{d.u32(a0+0x18):08X}"
                             f" overlay(+20)=0x{d.u32(a0+0x20):08X}")
                elif pc == 0x00285E80 or pc == 0x002853D0:
                    line += f"  count(+8)={d.u32(a0+0x8)}"
            print(line, flush=True)
            d.ok({"cmd": "resume", "cpu": "ee"})
            time.sleep(0.05)
    finally:
        for spec in specs:
            addr = spec.partition(":")[0]
            try:
                d.ok({"cmd": "remove_breakpoint", "cpu": "ee", "address": int(addr, 0)})
            except Exception:
                pass
        print(f"done: {hits} hits", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
