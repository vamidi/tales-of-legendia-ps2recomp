#!/usr/bin/env python3
"""PCSX2 side of the unit-1 post-fade wait (FUN_00204240, 0x20439C..0x204400).

The port parks at 0x2043F0 forever after the first fade. That loop waits for
`FUN_001520F0()` to return 1 (byte +0x1F2 of the object `FUN_001520C0` returns) for
at most 90 yields. This probe samples both sides in the reference:

  A 0x002043A4  after FUN_001520C0 returns v0 = object  -> read v0 and v0+0x1F2
  B 0x00204400  loop exit                               -> read s0 (iterations) and mode s1+0x14

Usage: python tools/ref_unit_wait.py [seconds] [maxRecords]
"""
import json
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 21512
P_OBJ = 0x002043A4
P_EXIT = 0x00204400


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

    def read(self, addr, length):
        r = self.ok({"cmd": "read_memory", "cpu": "ee", "address": addr, "length": length})
        return bytes.fromhex(r.get("hex", ""))


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    max_records = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    d = Dbg()
    d.ok({"cmd": "set_breakpoint", "cpu": "ee", "address": P_OBJ, "temporary": False})
    d.ok({"cmd": "set_breakpoint", "cpu": "ee", "address": P_EXIT, "temporary": False})
    print("breakpoints armed (0x2043A4, 0x204400)", flush=True)
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
            n += 1
            if pc == P_OBJ:
                obj = g.get("v0", 0)
                status_byte = -1
                if 0x00100000 <= obj < 0x02000000:
                    b = d.read(obj + 0x1F0, 8)
                    if len(b) >= 8:
                        status_byte = b[2]
                print(f"t={time.time()-t0:6.2f} A pc=0x{pc:08X} s0={g.get('s0',0):3d} obj=0x{obj:08X} "
                      f"status(+1F2)={status_byte}", flush=True)
            else:
                loader = g.get("s1", 0)
                mode = -1
                if 0x00100000 <= loader < 0x02000000:
                    b = d.read(loader + 0x14, 4)
                    if len(b) >= 4:
                        mode = int.from_bytes(b, "little")
                print(f"t={time.time()-t0:6.2f} B pc=0x{pc:08X} s0={g.get('s0',0):3d} loader=0x{loader:08X} "
                      f"mode={mode}", flush=True)
            d.ok({"cmd": "resume", "cpu": "ee"})
            time.sleep(0.005)
    finally:
        for a in (P_OBJ, P_EXIT):
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


if __name__ == "__main__":
    sys.exit(main())
