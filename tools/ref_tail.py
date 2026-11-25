#!/usr/bin/env python3
"""Reference side of FUN_00204240's mode==1 tail: which arm runs?

Both arms are reached by fall-through/branch inside one recompiled row, so the port cannot hook them, but
PCSX2 can break on any address:

  0x00204444  jal FUN_00285E80   -> the mode==1 SCENE START arm
  0x00204460  jal FUN_00164B90   -> the other arm (teardown, used when mode != 1 at 0x20442C)
  0x002044A0  the tail's return

On each hit it prints the branch inputs: f20 (the fade accumulator compared at 0x204418) and the loader
object's +0x14 mode word. That is the comparison the port needs.

Usage: python tools/ref_tail.py [seconds] [maxRecords]
"""
import json
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 21512
SCENE = 0x00204444
TEARDOWN = 0x00204460
RETURN = 0x002044A0


class Dbg:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
        self.s.settimeout(15)
        self.buf = b""

    def cmd(self, req):
        self.s.sendall((json.dumps(req) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.s.recv(1 << 20)
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
        return {r["name"]: int(r["value"], 16) for r in d.get("GPR", {}).get("regs", [])}

    def fprs(self):
        d = self.ok({"cmd": "read_registers", "cpu": "ee", "category": 2}).get("data", {})
        out = {}
        for key in ("FPR", "Fpu"):
            if key in d:
                for r in d[key].get("regs", []):
                    out[r["name"].lower()] = int(r["value"], 16)
        return out

    def read(self, addr, length):
        r = self.ok({"cmd": "read_memory", "cpu": "ee", "address": addr, "length": length})
        return bytes.fromhex(r.get("hex", ""))


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 40.0
    max_records = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    d = Dbg()
    for a in (SCENE, TEARDOWN, RETURN):
        d.ok({"cmd": "set_breakpoint", "cpu": "ee", "address": a, "temporary": False})
    print("tail breakpoints armed (0x204444 scene / 0x204460 teardown / 0x2044A0 return)", flush=True)
    d.ok({"cmd": "resume", "cpu": "ee"})
    t0 = time.time()
    n = 0
    try:
        while time.time() - t0 < seconds and n < max_records:
            st = d.status()
            if not st.get("paused"):
                time.sleep(0.01)
                continue
            g = d.gprs()
            pc = int(st.get("pc", "0x0"), 16)
            s1 = g.get("s1", 0)
            mode = -1
            if 0x00100000 <= s1 < 0x02000000:
                b = d.read(s1 + 0x14, 4)
                if len(b) >= 4:
                    mode = int.from_bytes(b, "little")
            f = d.fprs()
            f20 = f.get("f20", -1)
            n += 1
            name = {SCENE: "SCENE-START", TEARDOWN: "TEARDOWN", RETURN: "RETURN"}.get(pc, f"0x{pc:08X}")
            print(f"t={time.time()-t0:6.2f} {name:12s} powd={f20 & 0xFFFFFFFF:#010x} f20={struct_f(f20)} "
                  f"s1=0x{s1:08X} mode={mode} s0={g.get('s0',0)}", flush=True)
            d.ok({"cmd": "resume", "cpu": "ee"})
            time.sleep(0.005)
    finally:
        for a in (SCENE, TEARDOWN, RETURN):
            try:
                d.ok({"cmd": "remove_breakpoint", "cpu": "ee", "address": a})
            except Exception:
                pass
        try:
            d.ok({"cmd": "resume", "cpu": "ee"})
        except Exception:
            pass
        print(f"done: {n} records", flush=True)
    return 0


def struct_f(bits):
    import struct

    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


if __name__ == "__main__":
    sys.exit(main())
