#!/usr/bin/env python3
"""Capture the reference emulator's RAM at the three loader-unit boundaries.

The port dumps the same milestones with

    TOL_DUMP_UNITS="0x480000,0x200000,logs\\ramdump"

so the two sets are comparable guest-state to guest-state instead of by wall
clock. This writes:

    <outdir>/ref_unit1.bin      breakpoint at 0x204240 entry, mode(+0x14) == 1
    <outdir>/ref_unit1_ret.bin  at the caller's continuation (first insn after the jal)
    ... and the same for modes 2 and 3

Usage:
    python tools/ref_units.py [outdir] [base_hex] [len_hex] [timeout_s]
"""
import json
import os
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 21512
UNIT_FN = 0x00204240
MODES = (1, 2, 3)


class Dbg:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=15)
        self.s.settimeout(15)
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

    def paused(self):
        return bool(self.ok({"cmd": "status", "cpu": "ee"}).get("data", {}).get("paused"))

    def gprs(self):
        d = self.ok({"cmd": "read_registers", "cpu": "ee", "category": 0}).get("data", {})
        return {r["name"]: int(r["value"][-8:], 16)
                for r in d.get("GPR", {}).get("regs", [])}

    def u32(self, addr):
        r = self.ok({"cmd": "read_memory", "cpu": "ee", "address": addr, "length": 4})
        return int.from_bytes(bytes.fromhex(r.get("hex", "00000000")), "little")

    def dump(self, addr, length, path):
        data = bytearray()
        while len(data) < length:
            chunk = min(1024, length - len(data))
            r = self.ok({"cmd": "read_memory", "cpu": "ee",
                         "address": addr + len(data), "length": chunk})
            data += bytes.fromhex(r.get("hex", ""))
        with open(path, "wb") as fh:
            fh.write(bytes(data))
        print(f"  wrote {len(data)} bytes -> {path}", flush=True)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "logs/ramdump"
    base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x480000
    length = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x200000
    timeout = float(sys.argv[4]) if len(sys.argv) > 4 else 300.0
    os.makedirs(outdir, exist_ok=True)

    d = Dbg()
    d.ok({"cmd": "set_breakpoint", "cpu": "ee", "address": UNIT_FN, "temporary": False})
    print(f"breakpoint 0x{UNIT_FN:08X}; range 0x{base:08X}..0x{base + length:08X}; {timeout:.0f}s", flush=True)
    d.ok({"cmd": "resume", "cpu": "ee"})

    done = set()
    ret_target = None   # return address we are waiting for
    ret_mode = None
    t0 = time.time()
    while time.time() - t0 < timeout and len(done) < len(MODES):
        if not d.paused():
            time.sleep(0.1)
            continue
        g = d.gprs()
        pc = g.get("pc", 0)
        if ret_target is not None and pc == ret_target:
            d.dump(base, length, os.path.join(outdir, f"ref_unit{ret_mode}_ret.bin"))
            d.ok({"cmd": "remove_breakpoint", "cpu": "ee", "address": ret_target})
            done.add(ret_mode)
            ret_target = None
            ret_mode = None
        elif pc == UNIT_FN:
            a0 = g.get("a0", 0)
            mode = d.u32(a0 + 0x14)
            if mode in MODES:
                print(f"t={time.time() - t0:6.2f}s unit mode={mode} obj=0x{a0:08X}", flush=True)
                d.dump(base, length, os.path.join(outdir, f"ref_unit{mode}.bin"))
                ret_target = g.get("ra", 0)
                ret_mode = mode
                if ret_target:
                    d.ok({"cmd": "set_breakpoint", "cpu": "ee", "address": ret_target, "temporary": False})
            else:
                print(f"t={time.time() - t0:6.2f}s 0x{UNIT_FN:08X} with mode={mode} (ignored)", flush=True)
        d.ok({"cmd": "resume", "cpu": "ee"})
        time.sleep(0.05)

    try:
        d.ok({"cmd": "remove_breakpoint", "cpu": "ee", "address": UNIT_FN})
    except Exception:
        pass
    if ret_target:
        try:
            d.ok({"cmd": "remove_breakpoint", "cpu": "ee", "address": ret_target})
        except Exception:
            pass
    print(f"done: captured modes {sorted(done)}", flush=True)
    return 0 if len(done) == len(MODES) else 1


if __name__ == "__main__":
    sys.exit(main())
