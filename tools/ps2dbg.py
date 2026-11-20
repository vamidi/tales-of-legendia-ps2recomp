#!/usr/bin/env python3
"""Minimal PCSX2 DebugServer client (newline-delimited JSON over TCP, port 21512).

Usage:
  python ps2dbg.py status
  python ps2dbg.py pause | resume | step | stepover
  python ps2dbg.py read <addr> <len>          # hex dump + u32 words
  python ps2dbg.py write <addr> <hex>
  python ps2dbg.py dis <addr> [count]
  python ps2dbg.py eval <expr>
  python ps2dbg.py regs [category]
  python ps2dbg.py bp <addr> [cond]
  python ps2dbg.py bpdel <addr>
  python ps2dbg.py bplist
  python ps2dbg.py watch <addr> <end> [type] [cond]
  python ps2dbg.py un watch... (use watchdel <addr> <end>)
  python ps2dbg.py threads [cpu]
  python ps2dbg.py str <addr> [max]
  python ps2dbg.py raw <json>

Addresses accept 0x prefixes. One request per process, one connection.
"""
import json
import socket
import sys

HOST, PORT = "127.0.0.1", 21512


def send(req, timeout=10.0):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    s.settimeout(timeout)
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
    line = buf.split(b"\n", 1)[0].decode("utf-8", "replace")
    return json.loads(line)


def num(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def dump(hexstr, addr):
    data = bytes.fromhex(hexstr)
    print(f"{len(data)} bytes at 0x{addr:08X}")
    for off in range(0, len(data), 16):
        row = data[off:off + 16]
        hx = " ".join(f"{b:02x}" for b in row)
        words = []
        for w in range(0, len(row) - 3, 4):
            words.append(f"0x{int.from_bytes(row[w:w + 4], 'little'):08X}")
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        print(f"  +{off:04X}  {hx:<47}  {' '.join(words):<40} {asc}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1].lower()
    a = sys.argv[2:]

    if cmd == "status":
        r = send({"cmd": "status", "cpu": "ee"})
    elif cmd in ("pause", "resume", "step", "stepover"):
        r = send({"cmd": "step_over" if cmd == "stepover" else cmd, "cpu": "ee"})
    elif cmd == "read":
        addr, ln = num(a[0]), num(a[1]) if len(a) > 1 else 64
        r = send({"cmd": "read_memory", "cpu": "ee", "address": addr, "length": ln})
        if r.get("ok"):
            dump(r.get("hex", ""), addr)
            return 0
    elif cmd == "dump":
        addr, ln, path = num(a[0]), num(a[1]), a[2]
        data = bytearray()
        step = 1024
        while len(data) < ln:
            chunk = min(step, ln - len(data))
            r = send({"cmd": "read_memory", "cpu": "ee", "address": addr + len(data), "length": chunk})
            if not r.get("ok"):
                print(json.dumps(r))
                return 1
            data += bytes.fromhex(r.get("hex", ""))
        open(path, "wb").write(bytes(data))
        print(f"wrote {len(data)} bytes from 0x{addr:08X} to {path}")
        return 0
    elif cmd == "write":
        addr = num(a[0])
        r = send({"cmd": "write_memory", "cpu": "ee", "address": addr, "data": a[1]})
    elif cmd == "dis":
        addr = num(a[0])
        cnt = num(a[1]) if len(a) > 1 else 16
        r = send({"cmd": "disassemble", "cpu": "ee", "address": addr, "count": cnt, "simplify": True})
        if r.get("ok"):
            for ins in r.get("instructions", []):
                print(ins if isinstance(ins, str) else json.dumps(ins))
            return 0
    elif cmd == "eval":
        r = send({"cmd": "evaluate", "cpu": "ee", "expression": " ".join(a)})
    elif cmd == "regs":
        req = {"cmd": "read_registers", "cpu": "ee"}
        if a:
            req["category"] = num(a[0])
        r = send(req)
    elif cmd == "bp":
        addr = num(a[0])
        req = {"cmd": "set_breakpoint", "cpu": "ee", "address": addr, "temporary": False}
        if len(a) > 1:
            req["condition"] = a[1]
        r = send(req)
    elif cmd == "bpdel":
        r = send({"cmd": "remove_breakpoint", "cpu": "ee", "address": num(a[0])})
    elif cmd == "bplist":
        r = send({"cmd": "list_breakpoints", "cpu": "ee"})
    elif cmd == "watch":
        addr = num(a[0])
        end = num(a[1]) if len(a) > 1 else addr + 4
        req = {"cmd": "set_memcheck", "cpu": "ee", "address": addr, "end": end,
               "type": a[2] if len(a) > 2 else "write", "action": "break"}
        if len(a) > 3:
            req["condition"] = a[3]
        r = send(req)
    elif cmd == "watchdel":
        addr = num(a[0])
        end = num(a[1]) if len(a) > 1 else addr + 4
        r = send({"cmd": "remove_memcheck", "cpu": "ee", "address": addr, "end": end})
    elif cmd == "watchlist":
        r = send({"cmd": "list_memchecks", "cpu": "ee"})
    elif cmd == "threads":
        r = send({"cmd": "get_threads", "cpu": a[0] if a else "ee"})
    elif cmd == "str":
        addr = num(a[0])
        mx = num(a[1]) if len(a) > 1 else 256
        r = send({"cmd": "read_string", "cpu": "ee", "address": addr, "max_length": mx})
    elif cmd == "raw":
        r = send(json.loads(" ".join(a)))
    else:
        print(f"unknown command {cmd}")
        return 2

    print(json.dumps(r, indent=1)[:4000])
    return 0 if r.get("ok", True) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001 - CLI
        print(f"error: {exc}")
        sys.exit(1)
