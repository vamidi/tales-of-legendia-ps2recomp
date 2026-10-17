# Analyzer discovery pass — findings (session 11)

Session goal (user-chosen): close the recompiler/discovery gap "properly" — teach the
analyzer to cover code gaps and computed/indirect entry points, then regenerate once.

This document records what the investigation found **before** any analyzer change was
made, because the result changes the priority: the boot stall is **not** caused by the
discovery gap, it is an **IOP-side handshake** the HLE IOP never produces.

---

## 1. The stall, reduced to one flag

The loader thread ends up in `FUN_00198c60` / `entry_00198c88` (both entered at runtime,
confirmed by an entry watch). Decoding `FUN_00198c60` exactly (note `0x198c78` is a
**branch-likely**, so its delay slot `lui $v0,0x39` runs only when the branch is taken):

```
0x198c60: addiu sp,sp,-0x10
0x198c64: lui   v0,0x39
0x198c68: sd    s0,0(sp)
0x198c6c: addiu v0,v0,0x4378      ; v0 = 0x394378
0x198c70: sd    ra,8(sp)
0x198c74: ld    v1,0(v0)          ; v1 = *(u64*)0x394378
0x198c78: bnel  v1,zero, exit     ; if already set -> leave the loop
0x198c7c: lui   v0,0x39           ; (delay, only when taken)
0x198c80: daddu s0,v0,zero        ; s0 = 0x394378
0x198c84: nop
0x198c88: jal   func_198C48       ; <- the "poll" step
0x198c90: ld    v0,0(s0)          ; v0 = *(u64*)0x394378
0x198c94: beqz  v0,0x198c88       ; spin
...
0x198cac: sd    1,0(v0)           ; *(u64*)0x394380 = 1
0x198cb0: j     ExitDeleteThread  ; thread exits
```

`FUN_00198c48` is not a hardware poll — it is a spin counter:

```
0x198c48: lui v1,0x39; addiu v1,v1,0x4320   ; v1 = 0x394320
0x198c50: ld  v0,0(v1); daddiu v0,v0,1
0x198c58: jr ra
0x198c5c: sd  v0,0(v1)                      ; ++*(u64*)0x394320
```

So the effect is exactly:

```c
while (*(u64*)0x394378 == 0) ++*(u64*)0x394320;
*(u64*)0x394380 = 1;
ExitDeleteThread();
```

`0x394378` is a **staged-init handshake flag**. The block `0x394378..0x3943D0` is six
16-byte records; each stage thread waits on its flag, sets the next one, and exits.

## 2. No EE code can write `0x394378`

Deterministic backward-slice census of **every** store instruction in the whole image
(`SLUS_212.01`) whose computed address lands in `[0x394370,0x3943D0)`:

| store | target | function |
|---|---|---|
| `sd` @ `0x198CAC` | `0x394380` | `FUN_00198c60` (spin, exit path) |
| `sd` @ `0x198D44` | `0x394390` | stage thread 2 |
| `sd` @ `0x198E48` | `0x3943A0` | stage thread 3 |
| `sd` @ `0x198F28` | `0x3943B0` | stage thread 4 |
| `sd` @ `0x199064` | `0x3943C0` | stage thread 5 |
| `sd` @ `0x19996C` | **`0x394378`** | **`FUN_00199938`** |
| `sd` @ `0x199970` | `0x394388` | `FUN_00199938` |
| `sd` @ `0x199974` | `0x394398` | `FUN_00199938` |
| `sd` @ `0x199978` | `0x3943A8` | `FUN_00199938` |
| `sd` @ `0x19997C` | `0x3943B8` | `FUN_00199938` |
| `sd` @ `0x199984` | `0x3943C8` | `FUN_00199938` |

So **`FUN_00199938` is the only writer of `0x394378`** (it writes all six flags).

And `FUN_00199938` is unreachable, proven four independent ways:

1. **No control transfer reaches it.** Exhaustive scan of every `j`/`jal`/branch in the
   image for targets in `[0x199900,0x19A600)`: the only calls to `0x199938` come from
   `0x1999A8` (inside `FUN_00199988`) and `0x19A570` (inside `FUN_0019a550`).
2. **No materialised address.** Exhaustive `lui`+`addiu`/`ori` pair scan for values in
   `[0x198000,0x19A600)` finds 19 sites (all callback entries such as `0x198BF8`,
   `0x198C20`, `0x198C60`) — none is `0x199938`, `0x199988`, `0x19A550` or `0x19A2B0`.
3. **No data pointer.** No 4-byte word in the image (in any segment form,
   `w & 0x1FFFFFFF`) points into `[0x199000,0x19C000)`. Also no file under `data/`
   contains the flag addresses `0x394378`/`0x394380` in any endianness.
4. **Never entered at runtime.** An entry watch on `lookupFunction` (which covers both
   generated calls and scheduler dispatches, hence also fall-through entries) recorded
   `0x199938`, `0x199988`, `0x19A550`, `0x19A2B0`, `0x19A1C8`, `0x19A1EC`, `0x19A250`
   as **never entered**, while `0x166490`, `0x19A068`, `0x19A120`, `0x198C60`,
   `0x198C88` *were* entered.

Its two callers (`FUN_00199988`, `FUN_0019a550`) are the two copies of "run the six
subsystem inits"; they are equally unreachable (the six subsystem inits `0x199A20`,
`0x199AF0`, `0x199BC0`, `0x199C90`, `0x199D60`, `0x199E30` only *read* the stage flags
and are only callable from those two).

**Conclusion:** the EE-side init-stage chain cannot start on its own. The value of
`*(u64*)0x394378` must arrive from **outside the EE** — the IOP side (SIF/DMA write into
EE RAM, or an equivalent handshake).

Supporting runtime evidence: every SIF DMA in the last run is **EE→IOP**
(`[SIF-SetDma] ent0 w0=0x3b6a80 w1=0x137a40 …`, `pc=0x117a04` / `0x1ace2c`). No IOP→EE
transfer occurs. The HLE path can write EE RAM (RPC replies land: `recv_before=30303033,2`
then the EE reads it), so synthesising the handshake is feasible — the question is only
where the trigger belongs.

## 3. Analyzer defects found along the way (real, but NOT the cause of the stall)

`data/map.csv` (the Ghidra export) is systemically unreliable, and
`loadGhidraFunctionMap` (`PS2Recomp/ps2xRecomp/src/lib/elf_parser.cpp:2115`) **parses the
`size` column into `sizeStr` and then discards it**, using only the row's `End`, clamped
to the section.

Measured on the current map (53221 rows):

* **31 rows whose `End` runs far past their body** (`(end-start) - size > 64`). Worst:
  `FUN_00256bc8` +109308, `FUN_00256d58` +108580, `FUN_00256cc0` +108360,
  `FUN_00257888/a0/b8/d0` +106056…+106008, `FUN_00257a10` +104952, `FUN_0025b728` +76004,
  `FUN_001978f0` +32228, `FUN_00255808` +17304, `FUN_001a5dc0` +12028, `FUN_00253f00` +4968 …
  Each becomes a 32 KB–109 KB generated "monolith" that linearly decodes (and misdecodes)
  every byte of its range. `FUN_001978f0` alone registers **5881** function-table slots
  for addresses no real function covers.
* **23 rows with `size` > `end-start`** (code the export truncated), e.g.
  `FUN_0020fe10` size=928 but extent=36 — 892 bytes of real code attributed to nothing.
* **1031 rows with an alignment-sized surplus (≤ 64 B)** — normal, must be left alone
  (this is why the fix must use a materiality threshold and a next-start clamp, or every
  one of those 1031 files gets rewritten).
* **Unattributed code regions** with no row at all. `[0x19A1EC,0x19A250)` and
  `[0x19A2C4,0x19A550)` decode as clean, well-formed MIPS — a thunk (`j 0x19A1C8`), a
  once-guard (`if (*0x3943D8 == 0) { *0x3943D8 = 1; call 0x1A1498; call 0x197940;
  *0x3943D8 = 0; }`), a lock/unlock pair (`j 0x1B6F38` / `j 0x1B6F50`) and a 0x1E8-byte
  subsystem init that registers callbacks 6 and 7. They exist in the recompiled world
  only as `case …: goto label_…` entries of the monolith, and nothing dispatches to them
  (no control transfer, no materialisation, no data pointer) — i.e. dead code, but real
  code, and a genuine discovery gap.

The monolith also **shadows** its whole range for any range-based query (a naive
"which function contains address X" returns the monolith for every address in
`[0x1978F0,0x19F710)`), which is how a discovery pass can silently conclude "that region
is already covered" while the region is only *named* inside a misdecoded blob.

### What the analyzer fix would be (for a later cycle)

1. In `loadGhidraFunctionMap`: keep `size`; after parsing, normalise extents —
   shrink a runaway `End` when `(end-start) - size > 64`, extend when `size > end-start`,
   never cross the next known start, clamp to the section. This leaves the 1031
   alignment rows untouched (no churn) and removes all 31 monoliths.
2. Add the missing discovery heuristics: (a) unattributed regions inside the executable
   segment, validated by decoding to a terminator; (b) **materialised code addresses**
   (`lui`+`addiu`/`ori` producing an address in the code section) as candidate entry
   points — the code-side sibling of `ScanDataFunctionPointerTables`. (b) is what
   discovers the game's runtime callback targets (e.g. `0x198BF8`, `0x198C20`), which are
   stored only in RAM.

Neither of these sets `0x394378`, so neither unblocks the boot.
