# 20 — R5900 Context Layout (EE / IOP Quick Reference)

> **Source:** PS2Recomp wiki `Emotion-Engine-(EE)-(outdated).md` — hardware summary retained, **the struct below is corrected** against the live `ps2xRuntime/include/ps2_runtime.h`.
> **Use when:** reading/writing `ctx->...` fields, debugging registers, or writing an override lambda.
>
> Deep hardware detail lives in `db-ps2-architecture.md`, `db-registers.md`, `01-ps2-hardware-bible.md`, `09-ps2tek.md`. This file is the **code-side** view.

---

## Emotion Engine (EE) — R5900

| Property | Value |
|----------|-------|
| Core | Custom MIPS core (R5900) by Sony/Toshiba |
| ISA | MIPS III + MIPS IV mix, heavily augmented |
| Key extension | **MMI** (MultiMedia Instructions) — integer SIMD |
| Clock | 294.912 MHz |
| Execution | Superscalar, up to 2 instructions/cycle under ideal conditions |
| GPRs | 32 × **128-bit** (full width used by MMI) |
| Special registers | `PC` (32-bit), `HI`/`LO` (64-bit mul/div), `HI1`/`LO1` (MULT1/DIV1), `SA` (QFSRV shift amount) |
| I-Cache | 16 KB, 2-way, 64-byte lines, LRF replacement |
| D-Cache | 8 KB, 2-way, 64-byte lines, LRF, **lockable lines** |
| Scratchpad (SPR) | 16 KB, fast on-chip, at virtual `0x70000000`, also DMAC-accessible |
| On-die peripherals | DMAC, 4 × 16-bit timers, GIF, VIF, VU0, VU1, IPU |

### Coprocessors

- **COP0 (System Control)** — exceptions, MMU/TLB, cache control, timers, performance counters. Manages virtual segments (`kuseg`, `kseg0`, `kseg1`, `ksseg`, `kseg3`) and a **48-entry TLB**. Key registers: `Status`, `Cause`, `EPC`, `ErrorEPC`, `BadVAddr`, `BadPAddr`.
- **COP1 (FPU)** — fast single-precision. **Deviates from IEEE 754**: no NaNs/Infinities, denormals flush to zero, rounding always toward zero (with quirks). 32 × single-precision `f0`–`f31`.
- **COP2 (VU0 interface)** — EE-side access to VU0 in **macro mode**.

### MMU / TLB

48-entry TLB mapping `kuseg`/`ksseg`/`kseg3` to physical RAM or scratchpad. Page sizes 4 KB – 16 MB. `kseg0` / `kseg1` are direct-mapped (cached / uncached respectively).

### Exception handling

Managed by COP0 with distinct vectors (Reset, TLB Refill, Interrupt, Debug, …) and two levels: **Level 1** (`EPC`) and **Level 2** (`ErrorEPC`).

---

## Input/Output Processor (IOP)

| Property | Value |
|----------|-------|
| Core | MIPS **R3000A** — a PSX CPU duplicate |
| Purpose | Slower I/O: pads, memory cards, sound, networking, USB, FireWire, CD/DVD control; PSX backward compatibility |
| Clock | PSX speed 33.8688 MHz in PSX mode; ~37.5 MHz in PS2 mode |
| ISA | MIPS I |
| Registers | 32 × 32-bit GPRs, `PC`, `HI`/`LO` |
| Memory | Own **2 MB** RAM, separate from EE main RAM; shared 4 MB BIOS ROM |
| Caches | Standard R3000A caches; cache control at `0xFFFE0000` |
| COP0 | Basic system control, exceptions, minimal MMU (segment mapping) |
| Memory model | Simpler than EE — `KUSEG` / `KSEG0` / `KSEG1` |
| Peripherals | IOP DMA, IOP timers (PSX + 3 × 32-bit), CDVD, SIO2, SPU2, DEV9, USB, FireWire |
| EE link | **SIF** (Subsystem Interface) — incl. DMA and shared memory |
| Interrupts | Own controller (PSX-like, expanded): VBLANK, CDVD, DMA, Timers, SIO, SPU2, SIO2, USB |

> In this project the IOP is emulated as a separate subsystem (`PS2Recomp/ps2xIOP/`), and IOP modules (`*.IRX`) are **never recompiled** — the runtime loads and executes them.

---

## `R5900Context` — verified layout

Declared in **`ps2xRuntime/include/ps2_runtime.h`** as `struct alignas(16) R5900Context`.

> ⚠️ The wiki's original struct is **out of date**. Notably: `hi`/`lo`/`hi1`/`lo1` are `uint64_t` here (not split `uint32_t`), and many fields were added (`vu0_acc`, `llbit`/`lladdr`, `in_delay_slot`/`branch_pc`, `cop2_ccr[32]`, `f_acc`, multiple VPU status/FBRST/TPC slots). Always read the header for the authoritative list.

```cpp
struct alignas(16) R5900Context
{
    // General Purpose Registers (128-bit)
    __m128i r[32];

    // Control registers
    uint32_t pc;          // Program counter
    uint64_t insn_count;  // Instruction counter (emulation state, not hardware)
    uint64_t hi, lo;      // HI/LO for mult/div results
    uint64_t hi1, lo1;    // Secondary HI/LO for MULT1/DIV1
    uint32_t sa;          // Shift amount register (QFSRV)

    // VU0 registers (macro mode, as seen from the EE)
    __m128   vu0_vf[32];
    uint16_t vi[16];
    float    vu0_q, vu0_p, vu0_i;
    __m128   vu0_r;
    __m128   vu0_acc;
    uint16_t vu0_status;
    uint32_t vu0_mac_flags;
    uint32_t vu0_clip_flags, vu0_clip_flags2;
    uint32_t vu0_cmsar0, vu0_cmsar1, vu0_cmsar2, vu0_cmsar3;
    uint32_t vu0_vpu_stat, vu0_vpu_stat2, vu0_vpu_stat3, vu0_vpu_stat4;
    uint32_t vu0_tpc, vu0_tpc2;
    uint32_t vu0_fbrst, vu0_fbrst2, vu0_fbrst3, vu0_fbrst4;
    uint32_t vu0_itop, vu0_top, vu0_info, vu0_xitop, vu0_pc;
    float    vu0_cf[4];

    // COP0 System control
    uint32_t cop0_index, cop0_random, cop0_entrylo0, cop0_entrylo1;
    uint32_t cop0_context, cop0_pagemask, cop0_wired;
    uint32_t cop0_badvaddr, cop0_count, cop0_entryhi, cop0_compare;
    uint32_t cop0_status, cop0_cause, cop0_epc, cop0_prid, cop0_config;
    uint32_t cop0_badpaddr, cop0_debug, cop0_perf;
    uint32_t cop0_taglo, cop0_taghi, cop0_errorepc;

    // LL/SC reservation state (NOT part of COP0 Status)
    uint32_t llbit;
    uint32_t lladdr;

    // Delay-slot tracking
    bool     in_delay_slot;
    uint32_t branch_pc;

    // COP2 control (VU0 integer + control)
    uint32_t cop2_ccr[32];

    // FPU (COP1)
    float    f[32];
    float    f_acc;   // FPU accumulator
    uint32_t fcr31;
};
```

### Constructor invariants (don't fight these)

| Field | Initial value | Why |
|-------|---------------|-----|
| `vu0_q` | `1.0f` | Q is normally initialised to 1.0 |
| `cop0_random` | `47` | Starts at max TLB index |
| `cop0_status` | `0x00010001` | **EIE \| IE** — matches libkernel's handoff state |
| `cop0_prid` | `0x00002E20` | R5900 CPU ID |
| `in_delay_slot` / `branch_pc` | `false` / `0` | No pending branch |

> `cop0_status = 0x00010001` is deliberate: `IE` (bit 0) and `EIE` (bit 16) are separate enables and guest code reads both — **libkernel's `StartThread` refuses to run while `IE` is clear.** Do not "simplify" this to `0x00010000`.

### Field groups at a glance

| Group | Prefix | Notes |
|-------|--------|-------|
| GPRs | `r[0..31]` | `__m128i`; use `getRegU32` / `setReturnU32` / `setReturnS32` / `setReturnU64` for lane 0 |
| Core | `pc`, `insn_count`, `hi/lo`, `hi1/lo1`, `sa` | 64-bit HI/LO pairs |
| VU0 | `vu0_*`, `vi[]` | Macro-mode visible state |
| COP0 | `cop0_*` | System control, TLB, counters |
| COP2 | `cop2_ccr[32]` | VU0 integer/control |
| COP1 | `f[32]`, `fcr31`, `f_acc` | No NaN/Inf, denormals flush to zero |
| Sync | `llbit`, `lladdr` | LL/SC reservation |
| Branch | `in_delay_slot`, `branch_pc`, `pc` | Delay-slot bookkeeping |

---

## Helpers you should use instead of touching lanes directly

| Helper | Purpose |
|--------|---------|
| `getRegU32(ctx, reg)` | Read lane 0 of a GPR (returns `0` for `$0` and out-of-range) |
| `setReturnU32(ctx, v)` | Write `$v0` |
| `setReturnS32(ctx, v)` | Write `$v0` as signed |
| `setReturnU64(ctx, v)` | Write a 64-bit return across `$v0`/`$v1` |
| `ctx->dump()` | Formatted dump of PC/HI/LO/HI1/LO1/SA, all 32 GPRs, Status/Cause/EPC |

> Register values live in **lane 0** of each `__m128i`. Lane 3..0 order in `dump()` is `[3][2]_[1][0]` — read it as a 128-bit hex word.

---

## Practical notes

- **`$0` is hardwired zero.** `getRegU32` enforces this; writes to `r[0]` are meaningless.
- **Delay slots are real.** `in_delay_slot` / `branch_pc` carry the branch bookkeeping — recompiled code depends on it. Never clear them manually.
- **LL/SC** uses `llbit` + `lladdr`, not COP0 bits. Breaking this silently breaks atomic ops and lock loops.
- **FPU is not IEEE.** Don't reason about PS2 floats with host float semantics; use the provided macros (see `02-mips-r5900-isa.md`).
- **`insn_count` is emulation state**, not a hardware register — useful for cheap timing heuristics.

---

## Cross-references

- MIPS→C++ translation & MMI semantics → `02-mips-r5900-isa.md`
- Instruction encodings → `db-isa.md`
- VU instruction set → `db-vu-instructions.md`
- Hardware registers → `db-registers.md`
- Full architecture → `db-ps2-architecture.md`
- Runtime overview → `17-ps2x-runtime.md`
