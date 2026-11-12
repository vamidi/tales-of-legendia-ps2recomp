# Part 1 — The PS2 Ecosystem

*A hands-on tutorial for the person porting **Tales of Legendia** (`SLUS_212.01`) to PC with PS2Recomp.*

> **Never written C?** Read [`part0-c-primer.md`](part0-c-primer.md) first. It teaches the language from zero using the same binary, and Part 1 assumes its vocabulary (`uint8_t *rdram`, `ctx->pc`, macros, `goto`).

---

## What you will be able to do after this tutorial

By the end of Part 1 you can, on this machine, with this ELF:

1. **Locate** any address the game uses — code, static data, `.bss`, scratchpad, hardware register — and say which subsystem owns it, using two independent tools that agree.
2. **Read** a real Tales of Legendia function in MIPS assembly — its stack frame, its delay slots, its `lui`/`addiu` address construction, and its calls and jumps (`jal`, `j`, `jr`, `jalr`, `beql`) — without a decompiler.
3. **Decode** an instruction word by hand from its bit fields, and explain why `0x70000C28` clears a register on an R5900 but is not a MIPS instruction at all.
4. **Name the layer** a given piece of behaviour belongs to — hardware, BIOS/kernel, SDK library, or game code — and therefore know *where a fix goes* before you write one.
5. **Investigate** a value you have never seen before (like the ready-flags at `0x00394378`) by asking five questions that the binary can answer.

**Out of scope for Part 1:** the recompiler's translation rules, the runtime's stub/syscall implementation, VU microcode authoring, GS packet decoding, and actually running the port. Those are Parts 2–4.

## Audience and prerequisites

You need:

- To read C and C++ at a "I know what a pointer is" level.
- To have run a command in PowerShell once.
- No prior MIPS, no prior PS2 knowledge, no prior reverse-engineering experience. Every term is defined the first time it is used.

You do **not** need to have built the runtime yet. Part 1 never compiles anything.

## Estimated time

Roughly **3–5 hours** for a first pass, plus the challenge. This is a provisional estimate from the size of the material, not a measured pilot — nobody has timed a reader on it yet. Read one module per sitting; each ends with a checkpoint you can actually run.

## Setup

Everything below is **read-only against your own game files**. The tutorial copies nothing, patches nothing, and never writes into `data/`. Nothing is installed, nothing is downloaded, and no credentials or cloud services are involved.

### What is on this machine already

| Thing | Version / path | Used for |
|---|---|---|
| Game ELF | `data/SLUS_212.01` — 2,994,608 bytes | every example |
| `ps2elf.py` | `tools/old/ps2elf.py` | ELF header, segments |
| `dismips.py` | `tools/old/dismips.py` | MIPS disassembly without Ghidra |
| Ghidra | instance `:8192`, project `ps2`, file `SLUS_212.01` | decompiler, cross-references |
| Python | 3.15.0 | the sandbox tool |
| Tutorial tool | `docs/tutorial/sandbox/inspect.py` (created with this tutorial) | address → region, byte dumps |

Check the three that are easy to get wrong:

```powershell
python --version
python tools\old\ps2elf.py data\SLUS_212.01 --header
```

Expected output (verified on this machine):

```
file            data\SLUS_212.01 (2994608 bytes)
type            2 (EXEC)
machine         0x0008  MIPS (EE, R5900)
flags           0x20924000
entry           0x00100008
program headers 2 at 0x34
section headers 8 at 0x2DB070
load span       0x00100000 .. 0x004E9B80  (4103040 bytes)
```

If `python` is not found, stop and fix that first — every checkpoint needs it.

### The sandbox

`docs/tutorial/sandbox/inspect.py` is a 109-line, standard-library-only reader for exactly the questions Part 1 asks. It is deliberately separate from the real tooling so you can break it without consequence. Re-run it as often as you like; it only ever opens the ELF for reading.

```powershell
python docs\tutorial\sandbox\inspect.py data\SLUS_212.01 --segments
```

```
TYPE   OFFSET     VADDR      FILESZ     MEMSZ      FLAGS
LOAD   0x00000080 0x00100000 0x002DAF80 0x003E9B80 rwx
LOAD   0x002DB000 0x004E9B80 0x00000000 0x00000000 rw-
```

Keep those two numbers in your head for the whole tutorial. `FILESZ` is what is *stored in the file*; `MEMSZ` is what must *exist in RAM* at run time. They differ by

```
0x003E9B80 - 0x002DAF80 = 0x0010EC00 = 1,097,728 bytes
```

which is the size of the program's uninitialised data — the `.bss`. That gap is the single most common source of "the address is in RAM but the bytes are not in the file" confusion. We return to it in Module 3.

**Cleanup:** delete `docs/tutorial/sandbox/` when you are done. There is nothing else to undo.

---

# Module 1 — The machine as a system

## 1.1 Concept: one console, two computers, three buses

A PlayStation 2 is not a games console with a graphics chip. It is a small multiprocessor system: an **Emotion Engine** (the main CPU, your game's code), an **I/O Processor** (a second, older CPU that runs the disc drivers, the controller driver, the sound driver), and a **Graphics Synthesizer** (a fixed-function rasteriser with no shader stage at all), plus two **Vector Units** that sit next to the Emotion Engine and do the maths the main CPU is bad at.

*Diagram (ASCII, not a screenshot):*

```
                      ┌───────────────────────────┐
   DVD drive ────────►│  IOP  (R3000A, 33.87 MHz)  │──► SPU2 ──► audio out
                      │  2 MB RAM, runs .IRX      │──► PAD / MEMCARD
                      └────────────┬──────────────┘
                                   │ SIF (SIF0/SIF1 DMA + SIF command regs)
                      ┌────────────┴──────────────┐
   EE main RAM ──────►│  EE (R5900, 294.9 MHz)     │
   32 MB RDRAM        │  16 KB scratchpad          │
                      │  VU0 (4 KB+4 KB)           │
                      │  VU1 (16 KB+16 KB) ────────┼──► GIF ──► GS ──► TV
                      └───────────────────────────┘
```

Read it as three rules:

1. **The EE cannot see the disc.** Every file read is a request to the IOP. That is why the IOP exists and why the port has to emulate it rather than translate it.
2. **The EE cannot see the TV.** It writes a display list into RAM and hands it to the GS through DMA and the **GIF** (Graphics Interface). Rendering is the GS's job; the EE only prepares packets.
3. **DMA is the spine.** Moving data — to the GS, to a VU, to the IOP — is done by DMA engines, not by the CPU copying words. A DMA transfer is a descriptor in memory plus a register write that starts it.

### The blocks, in the order you will meet them

**Emotion Engine (EE).** The CPU. It executes your game's code. Everything in Module 2 is about this.

**RDRAM — 32 MB.** All of it. Not 32 MB plus expansion; the whole machine has 32 MB of main memory, shared by the game's code, its data, its heaps, its textures waiting to be uploaded, and its audio buffers. The runtime's own constant is authoritative:

```cpp
// thirdparty/PS2Recomp/ps2xRuntime/include/runtime/ps2_memory.h
constexpr uint32_t PS2_RAM_SIZE = 32u * 1024u * 1024u;
constexpr uint32_t PS2_RAM_MASK = PS2_RAM_SIZE - 1u;
```

**Scratchpad (SP).** 16 KB of fast memory that lives *inside* the EE, not in RDRAM. No DMA can reach it; only the CPU can. Address `0x70000000`. Code uses it for DMA descriptors and for the inner loops where cache misses would hurt.

**Vector Unit 0 (VU0).** 4 KB of microprogram memory and 4 KB of data memory. It has two personalities: **macro mode**, where the EE issues VU instructions inline as if they were ordinary CPU instructions (COP2), and **micro mode**, where it runs its own little program like a coprocessor.

**Vector Unit 1 (VU1).** 16 KB + 16 KB, micro mode only, and it exists for one reason: to transform vertices and build GS packets at high speed. It talks to the GS through the VIF1 DMA channel.

**Graphics Synthesizer (GS).** 4 MB of embedded DRAM, 147.456 MHz, no programmable shaders. It draws triangles and sprites with a fixed pipeline and two texture units. You feed it packets; you do not write shaders for it.

**I/O Processor (IOP).** A MIPS R3000A at 33.8688 MHz with 2 MB of RAM. It runs small modules — `.IRX` files — that provide the file system, the pad driver, the memory card, the network stack and, in this game, the sound driver. It is a *different CPU architecture*: 32-bit registers, no MMI, no VU. Code written for it cannot run on the EE and vice versa.

**SPU2.** The sound chip: two cores, 48 hardware voices each, each core with 2 MB of sample RAM. Game code rarely touches it directly — it talks to a driver on the IOP, which talks to the SPU2.

**CD/DVD subsystem.** The drive, its controller, and the file systems on the disc (ISO 9660 and UDF here). Owned by the IOP.

**Timers and interrupts.** Registers at `0x10000010` upward (timers) and `0x1000F000` (the interrupt controller, INTC). These are what make the machine feel concurrent.

### Why this matters for the port, not just for trivia

A static recompiler translates **EE code**. It cannot translate:

- the IOP's code (different CPU → the port *interprets* the `.IRX` modules instead),
- the GS (no code → the port re-implements the rasteriser),
- the SPU2 (no code → the port decodes the sample format),
- the kernel (the game calls it via `syscall` → the port implements the kernel calls in C++).

So when the port misbehaves, the question is never "is the CPU emulation wrong" first. It is **"which of these five blocks is the one that did not happen?"** Module 4 gives you the decision procedure.

## 1.2 Worked example: follow one value across the system

In `data/SLUS_212.01` there is a function at `0x00199938` that does nothing but this (Ghidra decompile, instance `:8192`):

```c
void FUN_00199938(void)
{
  DAT_00394378 = 1;
  DAT_00394388 = 1;
  DAT_00394398 = 1;
  DAT_003943a8 = 1;
  DAT_003943b8 = 1;
  DAT_003943c8 = 1;
  return;
}
```

Six flags, 16 bytes apart, at the top of the data segment. This is what a "subsystem is ready" table looks like on this console: one 16-byte block per subsystem, flag at offset 0, and the other 12 bytes are the rest of that subsystem's state.

Now watch where that value actually lives. Address `0x00394378` is below the end of the file-backed part of the segment (`0x00100000 + 0x002DAF80 = 0x003DAF80`), so it is *stored in the file* — and it is stored as zero:

```powershell
python docs\tutorial\sandbox\inspect.py data\SLUS_212.01 --va 0x00394378 --dump 0x20
```

```
0x00394378 -> file offset 0x002943F8
00394378  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
00394388  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
```

So on the disc the flags are all zero, and at run time the game flips them to 1. Everything downstream — loading progress, thread release, the frame loop — is gated on that transition. One `sd` instruction against one address decides whether the console boots.

That is the level at which you will debug this port: not "the emulator is wrong", but "this 8-byte store at `0x00394378` never happened, find out why".

## 1.3 Practice 1 — classify six addresses

Use `inspect.py` and the memory map in Module 3. For each address, write down *which block owns it* and *whether the bytes come from the file*:

```
0x00100008
0x00394378
0x004E0000
0x70000000
0x1000F000
0x12000000
```

**Checkpoint (run these, compare):**

```
0x00100008  -> file-backed, first instruction of the entry point
0x00394378  -> file-backed, zero-initialised data (the flags above)
0x004E0000  -> inside PT_LOAD but PAST p_filesz  => .bss, not in the file
0x70000000  -> not in any PT_LOAD                  => scratchpad
0x1000F000  -> not in any PT_LOAD                  => EE register (INTC)
0x12000000  -> not in any PT_LOAD                  => GS register
```

If you got 4 of 6, you have the model. If `0x004E0000` fooled you, re-read the `FILESZ` vs `MEMSZ` paragraph in Setup — that distinction will cost you a day otherwise.

## 1.4 Why "the console has two computers" changes your debugging

Consider the sound. Legendia ships **KORG's** IOP sound driver (`KORGIVAG.IRX`, per `docs/04-executable.md`), not Sony's `.IRX`. The music is 30 MIDI sequences played by that driver out of 25.8 MB of sample banks. No part of that chain is EE code:

- the sequences are read by the IOP file system,
- the sampler is an IOP module,
- the samples go to the SPU2 from IOP RAM.

So "music does not play in the port" is **not** a recompiler bug, and no amount of staring at generated C++ will find it. It is an IOP/RPC/SPU2 question. You will meet this exact class of confusion later; Part 1's job is to make it impossible.

---

# Module 2 — The Emotion Engine and the MIPS R5900

## 2.1 Concept: what makes an R5900 not a MIPS

The R5900 is a MIPS III core with MIPS IV instructions added, plus Sony's own **MMI** ("Multimedia Instructions") extension. Three differences matter to you:

1. **The general-purpose registers are 128 bits wide**, and there are 32 of them (plus HI/LO and HI1/LO1). Most code uses only the low 64 bits; MMI instructions operate on all four 32-bit lanes at once.
2. **There is no branch prediction and no out-of-order anything.** Instructions execute one at a time, and a branch does not take effect until *after* the instruction that follows it. That instruction is the **delay slot**.
3. **Floating point lives in a separate file.** 32 × 32-bit registers in COP1, addressed `$f0`…`$f31`. VU0 macro mode is COP2 with its own 32 × 128-bit `vf` registers.

### The register file you will read

| Name | Number | Role (by convention, not by hardware) |
|---|---|---|
| `$zero` | 0 | Always 0. Writes are discarded. |
| `$at` | 1 | Assembler temporary — the compiler's scratch. |
| `$v0`, `$v1` | 2, 3 | Return values. |
| `$a0`–`$a3` | 4–7 | First four arguments. |
| `$t0`–`$t7` | 8–15 | Caller-saved temporaries. |
| `$s0`–`$s7` | 16–23 | Callee-saved — a function that uses these must save them. |
| `$t8`, `$t9` | 24, 25 | More temporaries; `$t9` is also where indirect calls are placed. |
| `$k0`, `$k1` | 26, 27 | Reserved for the kernel. User code must not touch them. |
| `$gp` | 28 | Global pointer — base for the program's global data. |
| `$sp` | 29 | Stack pointer. |
| `$fp` (`$s8`) | 30 | Frame pointer (rarely used here). |
| `$ra` | 31 | Return address. |

`$gp` is set once at startup and never changes. In this binary the link step recorded it explicitly in `.reginfo`:

```powershell
python -c "d=open('data/SLUS_212.01','rb').read();import struct;print(hex(struct.unpack_from('<I',d,0x2DB06C)[0]))"
```

```
0x3e2bf0
```

So `$gp = 0x003E2BF0`, and the compiler is free to write `sw $a0, -0x7B44($gp)` instead of building a full 32-bit address. That is `$gp - 0x7B44 = 0x003DB0AC`. When you read this game's code you will see both styles — `lui`/`addiu` for hot globals and `$gp`-relative for the rest. Knowing `$gp`'s value is what lets you resolve the second style by hand.

## 2.2 Concept: three instruction formats, and how to decode one by hand

Every R5900 instruction is one 32-bit word. Almost all of them are one of these shapes:

```
R-type   31      26 25   21 20   16 15   11 10    6 5      0
         ┌─────────┬───────┬───────┬───────┬───────┬────────┐
         │ opcode  │  rs   │  rt   │  rd   │  sa   │ funct  │
         └─────────┴───────┴───────┴───────┴───────┴────────┘
         opcode = 0 means "look at funct"

I-type   31      26 25   21 20   16 15                     0
         ┌─────────┬───────┬───────┬────────────────────────┐
         │ opcode  │  rs   │  rt   │  immediate (16 bits)   │
         └─────────┴───────┴───────┴────────────────────────┘

J-type   31      26 25                                     0
         ┌─────────┬────────────────────────────────────────┐
         │ opcode  │        target (26 bits, ×4)            │
         └─────────┴────────────────────────────────────────┘
```

This is a *diagram of the encoding*, not byte values.

Now decode a real one. The very first instruction of Tales of Legendia is:

```
00100008  280C0070     clear    at
```

That is Ghidra's rendering. Note its convention: Ghidra prints the instruction's bytes **in memory order** (`28 0C 00 70` as they sit in the file), while the *word* you decode is the little-endian interpretation, `0x70000C28`. Strip the bits:

```
0x70000C28 = 0111 0000 0000 0000 0000 1100 0010 1000
opcode  = bits 31..26 = 011100 = 0x1C
rs      = bits 25..21 = 00000  = $zero
rt      = bits 20..16 = 00000  = $zero
rd      = bits 15..11 = 00001  = $at
sa      = bits 10..6  = 10000  = 16     <- NOT a shift amount, see below
funct   = bits  5..0  = 0x28   = PADDUW
```

That `16` in the `sa` field is the trap in this decode. On an ordinary MIPS instruction bits 10..6 are the shift amount, so a reader expects `0` here and will "correct" it to `0` if they only half-check. On an **MMI** instruction the field is a **fixed part of the opcode**: `PADDUW` is defined as opcode `0x1C`, bits 10..6 = `0b10000`, funct `0x28`. Read it as a shift and you will believe the instruction is `padduw` with a shift of 16, which does not exist.

You can confirm the decode mechanically rather than by eye — `docs/tutorial/sandbox/c/bits.c` does exactly this and prints:

```
word    0x70000C28
opcode  0x1C  (28)
rs      $0
rt      $0
rd      $1
sa      16
funct   0x28  (40)  = PADDUW
```

Opcode `0x1C` is not a standard MIPS opcode at all — it is Sony's **MMI** prefix, and `funct = 0x28` under it is `PADDUW`, "parallel add unsigned word": four independent 32-bit adds, no carry between lanes. Adding zero to zero in all four lanes is the cheapest way to zero a 128-bit register, and the compiler uses it everywhere. That is why the entry point begins with 32 of these:

```
00100008  280C0070     clear    at
0010000c  28140070     clear    v0
00100010  281C0070     clear    v1
...
```

**This is the single most important practical fact in Module 2:** if your disassembler only understands standard MIPS, it will print `.word 0x70000C28` and stop, because the first instruction of the program is already outside its vocabulary. That is exactly what the repo's capstone-based helper does:

```powershell
python tools\disasm_guest.py data\SLUS_212.01 0x100008 --count 4
```

```
(no output — capstone stops at the first unknown opcode)
```

Use `tools/old/dismips.py` (which decodes more of this game's dialect) or Ghidra instead, and prefer Ghidra when you need names:

```powershell
python tools\old\dismips.py data\SLUS_212.01 --va 0x00100008 4
```

```
0x00100008  0x00000088  70000c28  .word 0x70000C28
0x0010000C  0x0000008C  70001428  .word 0x70001428
0x00100010  0x00000090  70001c28  .word 0x70001C28
0x00100014  0x00000094  70002428  .word 0x70002428
```

Both tools are honest about their limits; neither is "wrong". Knowing *which* one is lying to you is the skill.

## 2.3 Concept: delay slots, and why they make assembly look shuffled

The instruction after a branch or jump executes **before** the branch is taken. It is called the **delay slot** and it is not optional. Compilers fill it with useful work, which is why a function call looks like this:

```
00199e88  5C51040C     jal      0x00114570
00199e8c  0000048E     _lw      a0, 0x0(s0)      <- runs BEFORE the jump
```

Ghidra marks delay-slot instructions with a leading underscore. So the real order is:

1. load `$a0` from `[s0]`,
2. *then* jump to `0x00114570`.

Compilers do this so the callee receives an argument that was fetched while the jump was in flight. Branch-delay slots work the same way:

```
00199e60  15004014     bne      v0, zero, 0x00199eb8
00199e64  3900153C     _lui     s5, 0x39
```

means: compute `s5 = 0x39 << 16` **first**, then test and branch.

**Why you must care in a recompiler project:** a translated function cannot just emit `if (...) goto label;` at the address of the branch — it has to emit the delay-slot instruction's effect *before* the control transfer, or the guest's registers will be one instruction stale at every single branch. When a port has a bug that appears "one step behind" the original, a mishandled delay slot is the first thing to suspect.

## 2.4 Concept: the stack frame, and how to recognise a function

Almost every non-trivial function in this game starts the same way. Here is `0x00199E30`, a real function, straight from Ghidra:

```
00199e30  27BDFFC0     addiu    sp, sp, -0x40        ; make 0x40 bytes of frame
00199e34  FFB60030     sd       s6, 0x30(sp)         ; save callee-saved regs
00199e38  3C160039     lui      s6, 0x0039
00199e3c  FFB00000     sd       s0, 0x0(sp)
00199e40  26C34380     addiu    v1, s6, 0x4380       ; v1 = 0x00394380
00199e44  FFB10008     sd       s1, 0x8(sp)
00199e48  FFB20010     sd       s2, 0x10(sp)
00199e4c  FFB30018     sd       s3, 0x18(sp)
00199e50  FFB40020     sd       s4, 0x20(sp)
00199e54  FFB50028     sd       s5, 0x28(sp)
00199e58  FFBF0038     sd       ra, 0x38(sp)         ; save the return address
00199e5c  DC620000     ld       v0, 0x0(v1)          ; v0 = *(u64*)0x00394380
00199e60  15004014     bne      v0, zero, 0x00199eb8 ; already done? bail out
00199e64  3900153C     _lui     s5, 0x39             ; delay slot
```

Read the prologue as a sentence: *"reserve 0x40 bytes, save every callee-saved register I am about to use, save `$ra` at the top of the frame, then check a flag."*

Four things to internalise:

1. **`$ra` is saved because the function calls other functions.** If it did not call anything, `$ra` would never be clobbered and the save would be unnecessary. `sd ra, 0x38(sp)` in a prologue is a reliable signal: *there is a `jal` in here*.
2. **Frame sizes are multiples of 16.** The ABI requires 16-byte stack alignment because the 128-bit registers need it. `-0x40` is legal; `-0x38` would be a compiler bug.
3. **`lui` + `addiu` is how a 32-bit constant is built.** `lui s6, 0x39` puts `0x00390000` in the register (upper half, lower 16 bits zeroed); `addiu v1, s6, 0x4380` adds `0x4380` → `0x00394380`.
4. **`sd`/`ld` are 64-bit, `sw`/`lw` are 32-bit, `sq`/`lq` are 128-bit.** The flags at `0x00394380` are read with `ld`, so they are treated as 64-bit values even though only the low byte matters.

### The sign-extension trap in `lui` + `addiu`

`addiu` sign-extends its 16-bit immediate. So `addiu v0, s6, 0x4380` is fine (`0x4380 < 0x8000`), but

```
lui   a0, 0x0039
addiu a0, a0, 0x9437        ; wrong: 0x9437 sign-extends to 0xFFFF9437
```

would give `0x0038FFFF9437` truncated to `0x00389437`, not `0x00399437`. The compiler knows this and emits `lui a0, 0x003A` instead — one higher. When you reconstruct an address by hand and it lands 0x10000 off, this is why. **Rule: if the low half is `>= 0x8000`, the `lui` value is one greater than the address's top half.**

## 2.5 Concept: jumps and calls — `j`, `jal`, `jr`, `jalr` — and the branches

Control transfer comes in two families, and the difference decides how hard the port has to work:

- **Direct** — the target is *in the instruction*. `j`, `jal`, `beq`, `bne` and friends.
- **Indirect** — the target is *in a register*, computed at run time. `jr`, `jalr`.

### `j` — unconditional jump, no link

Target is a 26-bit field, shifted left twice, OR-ed into the top 4 bits of `PC+4` (so a `j` cannot leave the 256 MB region it started in). The word at `0x00100144`:

```
0010012c  0F004430     andi     a0, v0, 0xf
00100130  06008010     beq      a0, zero, 0x0010014c
00100134  00000000     _nop
00100138  000040A0     sb       zero, 0x0(v0)
0010013c  01004224     addiu    v0, v0, 1
00100140  00000000     nop
00100144  4B000408     j        0x0010012c
00100148  00000000     _nop
0010014c  FFFF043C     lui      a0, 0xffff
```

Read the shape, not the purpose: `j` backwards to `0x10012c`, `beq` forwards out of the loop. That is a **`do { } while (...)` loop**, and `j` is how the compiler closes it. (`j` is never used for calls — it does not touch `$ra`.)

### `jal` — jump and link, i.e. a call

Identical target arithmetic, plus one side effect: **`$ra = PC + 8`**, not `PC + 4`, because the PC has already advanced past the delay slot by the time the link happens.

```
00199e88  5C51040C     jal      0x00114570
00199e8c  0000048E     _lw      a0, 0x0(s0)
```

The callee returns with `jr $ra`, which lands on `0x00199E90` — the instruction **after** the delay slot. Get this wrong in a translator and every function returns one instruction early.

`jal` is by far the most common transfer in this binary: a linear sweep of the file-backed segment finds tens of thousands of `jal` words (treat that as an order of magnitude — the segment also contains embedded data, so a raw sweep overcounts).

### `jr` — jump through a register

`jr $ra` is the return. `jr $t9` is a call whose target could not be known at compile time. `jr $v0` is a computed jump (state machines, jump tables).

```
00114728  0800E003     jr       ra
0011472c  00000000     _nop
```

### `jalr` — jump and link through a register

`jalr rd, rs` puts the return address in `rd` (default `$ra`) and jumps to `rs`. Ghidra prints `jalr v1` when `rd` is `$ra`. Here is a real indirect call, with the `$gp` juggling that surrounds it:

```
00101610  2D808003     move     s0, gp            ; save the current $gp
00101614  84B8BC8E     lw       gp, -0x477c(s5)   ; load the callee's $gp
00101618  09F86000     jalr     v1                ; call through $v1
0010161c  6452248E     _lw      a0, 0x5264(s1)    ; first argument, in the delay slot
00101620  2DE00002     move     gp, s0            ; restore $gp after the call
```

Two lessons in five instructions: the delay slot carries the argument, and `$gp` is *not* globally constant — position-independent code reloads it per module, which is why the surrounding `move`/`lw`/`move` exists.

### The branches, and the `*l` variants that skip their delay slot

`beq`, `bne`, `blez`, `bgtz`, and the REGIMM family `bltz` / `bgez` compare and branch. Their displacement is a signed 16-bit **word** count relative to `PC+4` (Part 1 Appendix A.4).

`beql` and `bnel` are the same, except: **when the branch is not taken, the delay slot does not execute.** The hardware *nullifies* it. Here is `bnel` in the wild:

```
00100448  10000224     li       v0, 0x10
0010044c  1B00C254     bnel     a2, v0, 0x001004bc
00100450  9483828F     _lw      v0, -0x7c6c(gp)   ; skipped if a2 == v0
00100454  9483828F     lw       v0, -0x7c6c(gp)   ; ...so load it again
```

Why is the load written twice? Because it *has* to be: if `a2 == v0` the branch is not taken, the slot is nullified, and `v0` still holds `0x10`. The compiler duplicated the load to cover the not-taken path. That duplication is your visual proof that nullification is real, and it is why a translator cannot treat `beql` as `beq` with a slot.

`b` (unconditional branch) is a pseudo-instruction: `beq $zero, $zero, target`. It has a delay slot and no link, so it is used for forward gotos where `j`'s page restriction would be awkward.

### Tail calls: a call that never comes back

```
0010045c  1A000010     b        0x001004c8
```

Also common: at the end of a function, `jr $t9` after the frame has been torn down. That is a **tail call** — the callee's `jr $ra` returns directly to the *original* caller, because `$ra` was never overwritten. Useful to recognise: there is no `jal` for it, so it looks like a jump out of nowhere.

### Why this matters for the port: three destinations, three fates

| Form | Target known statically? | What the recompiler does |
|---|---|---|
| `jal target` | yes | emits a call through the runtime's dispatch (`dispatchGuestBranch`) to the table slot for `target` |
| `j target` | yes | same, without a link |
| `jr $ra` | yes, semantically | returns to the caller |
| `jr $t9`, `jalr $t9` | **no** | runtime lookup of a *computed* address; if no slot holds a function, the port reports a missing function |
| `beql`/`bnel` | yes | branch **plus** conditional execution of the delay slot |

And the case that ate days in this project:

```
0x205F90 : lw t9, 0(a0) ; lw t9, 0x1C(t9) ; jr t9
```

That is C++ virtual dispatch: load the vtable pointer from the object, load slot 7 (`0x1C/4`) from the vtable, jump. Nothing in the binary names the target, so **static analysis cannot enumerate it** — which is precisely why this project ships an `entry_points` list, why Ghidra leaves holes where a vtable is the only reference, and why a missing function at a vtable address has to be fixed by regenerating with that address, not by staring at the translation.

## 2.6 Concept: exceptions and syscalls

`syscall` (word `0x0000000C`, formatted as `syscall 0x0`) is not a function call. It is a trap: the CPU switches to kernel mode and jumps to a fixed exception vector, where the BIOS/kernel decides what to do. The game uses it for every kernel service — threads, semaphores, file I/O, timers.

The convention on this console: **`$v1` holds the syscall number**, arguments go in `$a0`–`$a3`, the result comes back in `$v0`.

Here is a real kernel stub from the game, at `0x00114720`:

```
00114720  44000324     li       v1, 0x44
00114724  0C000000     syscall  0x0
00114728  0800E003     jr       ra
0011472c  00000000     _nop
```

Four instructions: put `0x44` in `$v1`, trap, return. `0x44` is **WaitSema** (confirm in `.opencode/skills/ps2-recomp/resources/db-syscalls.md`: `0x44 | WaitSema | a0=sema_id | v0=status`). So this "function" is not a function at all — it is the game's own copy of the kernel's API surface, generated at link time.

**The porting consequence, stated once and remembered forever:** a recompiler faithfully translates `li v1, 0x44; syscall`. It cannot invent what `WaitSema` does. If the runtime's `WaitSema` implementation is wrong, the translated code is still *correct* and the game still hangs. That is the "is the translation wrong, or is the environment incomplete?" question, and answer #4 below is how you decide.

## 2.7 Practice 2 — decode six words, then read a real function

**Part A.** Decode these words by hand (opcode/rs/rt/rd/sa/funct as applicable), then check against Ghidra:

```
0x27BDFFC0
0xFFBF0038
0x3C160039
0x26C34380
0x03E00008
0x0000000C
```

**Checkpoint:**

| word | decode | meaning |
|---|---|---|
| `0x27BDFFC0` | `addiu $sp,$sp,-0x40` | 64-byte stack frame |
| `0xFFBF0038` | `sd $ra,0x38($sp)` | save return address at frame top |
| `0x3C160039` | `lui $s6,0x0039` | `$s6 = 0x00390000` |
| `0x26C34380` | `addiu $v1,$s6,0x4380` | `$v1 = 0x00394380` |
| `0x03E00008` | `jr $ra` | return |
| `0x0000000C` | `syscall` | trap to the kernel |

**Part B.** Read `FUN_00198C60` and answer: does it call anything inside its loop, and what does it wait for?

```c
void FUN_00198c60(void)
{
  while (DAT_00394378 == 0) {
    FUN_00198c48();
  }
  DAT_00394380 = 1;
  ExitDeleteThread();
  return;
}
```

**Checkpoint:** it spins while `*(u64*)0x00394378 == 0`, calling `FUN_00198C48` (a yield/sleep — a busy loop that calls *something* is nearly always cooperative yielding), then sets the next flag at `+0x8` and kills its own thread. It waits for the flag from Module 1. If that flag never becomes 1, **this thread never dies**, and the loader waits on *it*. Two lines of C, one boot hang.

## 2.8 The rest of the R5900, in the order you will need it

You do not have to memorise the ISA. You need to recognise these families:

**Integer.** `add`/`addu`/`sub`/`subu` (use the `u` forms for addresses; overflow traps are rare), `and`/`or`/`xor`/`nor`, `sll`/`srl`/`sra` and the variable forms, `slt`/`sltu`/`slti`, `mult`/`multu`/`div`/`divu` with `mfhi`/`mflo`, `movz`/`movn`.

**Load/store.** `lb`/`lbu`/`lh`/`lhu`/`lw`/`lwu`/`ld`, `sb`/`sh`/`sw`/`sd`, plus `lq`/`sq` for 128-bit, and the unaligned pair `lwl`/`lwr` / `swl`/`swr`.

**Branches.** `beq`/`bne`/`blez`/`bgtz`, the likely variants `beql`/`bnel`, and the REGIMM forms `bltz`/`bgez`. The `*l` variants nullify the delay slot when not taken — a detail a translator must honour; §2.5 shows the compiler's compensating duplicate load that proves it. `bgezal`/`bltzal` (branch-and-link) exist in the ISA, but I found no use of them **inside an analysed function** in this build: the handful of matching word patterns sit in data, outside every Ghidra function.

**Jumps.** `j` (unconditional, no link), `jal` (direct call), `jr` (register), `jalr` (register call) — all four in §2.5, with `$ra = PC + 8`, the 26-bit page restriction, tail calls, and what each becomes in the port.

**MMI.** The reason R5900 is interesting: `padduw`/`paddsw`/`psubuw`, `pcpyld`/`pcpyud`, `pextlw`/`pextuw`, `pmaddw`, `pdivw`, `psllh`/`psrlh`, `pand`/`por`/`pxor`, `pmfhi`/`pmflo`. All operate on the full 128-bit register as four 32-bit lanes or eight 16-bit lanes.

**COP0** — the CPU control registers: `Status`, `Cause`, `EPC`, `BadVAddr`, `Count`, `Compare`, `Config`. Read with `mfc0`, written with `mtc0`. The runtime's `R5900Context` carries all of them; `patch_cop0 = true` in `data/config.toml` exists because some of them (the `Count` timer, the `Config` cache bits) do not mean on a PC what they meant on a console.

**COP1** — the FPU: 32 × 32-bit `$f` registers, which can be paired for `lwc1`/`swc1` and for double-precision ops. Note `cprmask[1] = 0xFFFFFFFF` in this ELF's `.reginfo`: the program uses the whole FPU register file, so every function must be assumed to clobber floating-point state.

**COP2** — VU0 in macro mode. `vadd`, `vmul`, `vcall`, `vif`-style transfers. When you see `cop2` instructions in EE code, the VU is being driven inline, and the port must execute them in `ps2vu` rather than translate them away.

**Endianness.** The machine is little-endian, and so is everything on the disc. Every tool you will use prints instructions differently, and this is a real source of confusion:

| What you are looking at | Example | Meaning |
|---|---|---|
| Ghidra's byte column | `280C0070` | the four bytes **in memory order**, i.e. `28 0C 00 70` |
| the file, hex-dumped | `28 0c 00 70` | the same four bytes |
| the instruction word | `0x70000C28` | those bytes read little-endian |
| `dismips.py` output | `70000c28` | the word, not the bytes |

All four describe one instruction. Ghidra does **not** print a big-endian word — it prints memory order, which happens to look reversed relative to the word. When a hex dump and a disassembly disagree, decide first whether you are comparing *bytes* or a *word*.

## 2.9 Checkpoint summary for Module 2

You are done with Module 2 when you can, without help:

- look at `addiu sp,sp,-0x40` and say "this function has a 64-byte frame and almost certainly saves `$ra`";
- see `jal X` followed by an underscored instruction and know which executes first, and that `$ra` becomes `PC+8`;
- tell `j` from `jal` from `jr` from `jalr` at a glance, and say which of them the port can resolve statically;
- see `beql`/`bnel` and remember the delay slot may be **skipped**;
- see `lui r,0x39; addiu r,r,0x4380` and compute `0x00394380` in under five seconds;
- see `li v1,0x44; syscall` and know it is a kernel call, not a library call;
- explain why `0x70000C28` defeats a general-purpose MIPS disassembler.

---

# Module 3 — Memory architecture

## 3.1 Concept: the map

The EE sees a 4 GB address space that is mostly empty. Learn these ranges and 80 % of "what is this address" questions answer themselves.

*Table assembled from the runtime's own constants (`runtime/ps2_memory.h`, marked ✓) and the standard EE core map. Entries without a ✓ come from the published EE map and were not verified against this build in this session.*

| Range | What it is | |
|---|---|---|
| `0x00000000`–`0x01FFFFFF` | Main RAM — 32 MB RDRAM | ✓ |
| `0x10000000`–`0x1000FFFF` | EE registers: timers, DMA, INTC | ✓ |
| `0x10003000` | GIF registers (graphics interface) | |
| `0x10003800` / `0x10003C00` | VIF0 / VIF1 registers | |
| `0x10008000`+ | DMA channel registers, one 0x1000 block per channel | |
| `0x1000F000` | INTC — interrupt controller status/mask | |
| `0x11000000` | VU0 microprogram memory, 4 KB | ✓ |
| `0x11004000` | VU0 data memory, 4 KB | ✓ |
| `0x11008000` | VU1 microprogram memory, 16 KB | ✓ |
| `0x1100C000` | VU1 data memory, 16 KB | ✓ |
| `0x12000000` | GS privileged registers | ✓ |
| `0x1FC00000`–`0x1FFFFFFF` | BIOS ROM, 4 MB | ✓ |
| `0x70000000` | Scratchpad, 16 KB | ✓ |
| `0x80000000`–`0x9FFFFFFF` | KSEG0 — cached direct-mapped view of physical 0–512 MB | |
| `0xA0000000`–`0xBFFFFFFF` | KSEG1 — **uncached** direct-mapped view of the same | |
| `0xC0000000`–`0xFFFFFFFF` | KSEG2 — TLB-mapped, used by the kernel | ✓ (base constant) |

The scratchpad has a second name: `0xF0000000` (`PS2_SCRATCHPAD_ALIAS_BASE` in the runtime). The runtime checks both spellings, which is your first hint that **an address in this architecture is a window, not an identity.**

## 3.2 Concept: the same bytes, three addresses

The low 512 MB of the physical map is reachable at three different virtual addresses:

```
0x00000000 + X   physical, as the DMA engines and the IOP know it
0x80000000 + X   cached     — normal loads/stores, goes through the cache
0xA0000000 + X   uncached   — bypasses the cache, always hits RAM
```

Why uncached exists: DMA writes into RAM behind the CPU's back. If the CPU reads that buffer through the cache, it may read a stale line. So the driver reads the DMA buffer through `0xA0000000` (or issues a cache flush) — both appear in this game's code.

**Practical rule for debugging:** if you see the *same* value written to `0xA0001234` and "never visible" at `0x00001234`, you have found a cache-coherency assumption, not a memory bug. The runtime folds the top bits with `PS2_RAM_MASK` precisely so both names hit the same 32 MB array.

## 3.3 Concept: virtual address → file offset

Every question of the form "what is in the ELF at address X" reduces to one formula, valid for any `PT_LOAD` segment:

```
file_offset = p_offset + (va - p_vaddr)
```

For this ELF: `file_offset = 0x80 + (va - 0x00100000)`.

Two guards, and they are the whole of Module 3 in practice:

1. If `va` is not inside `[p_vaddr, p_vaddr + p_memsz)`, it is **not in this segment** — it is another part of the map (Module 3.1) or nothing.
2. If it *is* inside that range but `va - p_vaddr >= p_filesz`, it is **past the end of the file data**: `.bss`. The bytes do not exist on the disc; they are created as zero when the loader maps the segment.

`inspect.py` implements exactly this and refuses politely when neither holds. That is the tool doing you a favour: "not inside any PT_LOAD" is information.

Let me make you compute one by hand, the way you will have to when you have only a hex editor:

```
va = 0x00394378
offset = 0x80 + (0x00394378 - 0x00100000)
       = 0x80 + 0x00294378
       = 0x002943F8
```

and `inspect.py` agrees. Learn the formula; do not memorise offsets, because they change the moment the ELF is rebuilt.

## 3.4 Concept: the segments of *this* program

```
LOAD  file 0x00000080  vaddr 0x00100000  filesz 0x002DAF80  memsz 0x003E9B80  rwx
LOAD  file 0x002DB000  vaddr 0x004E9B80  filesz 0x00000000  memsz 0x00000000  rw-
```

Read that carefully, because it is unusual and it will confuse you at least once:

- There is **one** real segment, and it is marked `rwx` — read, write, execute in one blob. Console binaries do not have the separation a modern OS enforces. Code, string literals, and mutable globals are all in the same 4 MB range.
- Code starts at `0x00100000`. There is no `0x0` page; the loader maps the segment and jumps to `e_entry = 0x00100008`.
- The 8 bytes between `0x00100000` and `0x00100008` are two zero words; the program's first instruction is at the entry point, 8 bytes in.
- The file holds `0x2DAF80` bytes but `0x3E9B80` bytes must exist: the extra `0x10EC00` (1,097,728) bytes are `.bss`.
- The second `PT_LOAD` is **empty**. It is a marker, not a segment. Do not chase it.
- There is **no symbol table**: `.symtab` and `.strtab` exist as headers with size zero. So every function in the port has a name like `FUN_00199938` — an address, not a name. Ghidra's `map.csv` (11,604 functions, 53,221 records) is the substitute for the symbol table the linker threw away.

The compiler is stamped in `.comment`:

```
MW MIPS C Compiler (2.4.1.01)\0PlayStation2\0
```

Metrowerks. That matters practically: the code generation idioms (delay-slot filling, `padduw` for clearing, `gp`-relative globals) are Metrowerks', and they are consistent across the binary — so once you learn to read one function, you can read all 11,604.

## 3.5 Concept: stack, heap, static — told apart by address

Within the 32 MB of RDRAM, three things share the space and are distinguishable by *who allocated them* and *where they sit*:

- **Static data** — inside the ELF's load span: `0x00100000`–`0x004E9B80`. Its initial contents are in the file (up to `0x003DAF80`) or zero (`.bss`). Addresses here are compile-time constants and appear in the code as `lui`/`addiu` pairs or `$gp`-relative.
- **Stack** — the kernel places it. In this game the main thread's stack is at the top of RAM (`sp ≈ 0x01FFFxxx`), growing **downward**. Anything you see addressed as `N($sp)` is a local variable or a saved register.
- **Heap** — what `malloc`/`new` returns, from the kernel's heap at `0x00100000` upward or from the game's own allocator pools. Addresses here are *computed at run time* and do not appear as constants in the binary. If an address is above the ELF's load span and below the stack, and it is not a kernel structure, it is heap.

That gives you the last fence: **is this address a constant in the binary, or a run-time value?** Constants can be found by searching. Run-time values can only be found by watching the code that computes them.

## 3.6 Concept: memory-mapped registers behave nothing like memory

Reading `0x1000F000` is not a load; it is a question asked of the interrupt controller, and the answer changes between two identical instructions. Writing a register can start a DMA transfer, clear an interrupt, or reset a channel.

The consequences for a port:

- **Never cache a register read** across code that might change it.
- **Never assume a write is idempotent.** Writing `1` to an INTC status bit *clears* that bit; writing twice is not the same as writing once.
- **Never assume a read has no effect.** Some registers pop a FIFO.

This is why the runtime's `Load32`/`Store32` check the address before touching the RAM array — verified behaviour, not decoration:

```cpp
// runtime/ps2_memory.h
if (addr >= PS2_SCRATCHPAD_BASE && addr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE)) ...
```

## 3.7 The investigation procedure — five questions for any address

This is the reusable skill. Ask these in order and stop when you have an answer.

1. **Is it in the ELF?** `va` inside `[p_vaddr, p_vaddr+p_memsz)` of a `PT_LOAD`? → static code or data. Then check `p_filesz` for file-backed vs `.bss`.
2. **Is it a known hardware window?** `0x1000xxxx` EE regs, `0x1100xxxx` VU, `0x12000000` GS, `0x1FCxxxxx` BIOS, `0x7000xxxx` scratchpad. → hardware; the *offset within the block* names the register.
3. **Does it have the KSEG prefix?** Top three bits `100`/`101`/`110` → KSEG0/1/2. Strip them and go back to question 1 with the physical address.
4. **Is it in RAM but outside the ELF?** Above `0x004E9B80` and below the stack → heap, allocated at run time. Ask who allocated it, not where it is.
5. **Who touches it?** In Ghidra, cross-references. Writes tell you who produces the value; reads tell you who consumes it; the *width* of the access (`sd` vs `sw` vs `sb`) tells you the object's layout.

Question 5 is the one that actually solves problems, and it is the one beginners skip.

## 3.8 Worked example: applying the procedure to `0x00394378`

1. In the ELF? `0x00100000 <= 0x00394378 < 0x004E9B80` → yes. Below `0x003DAF80` → file-backed, and the file says zero.
2. Hardware window? No.
3. KSEG prefix? Top bits `000` → physical, no translation.
4. Outside the ELF? No.
5. Who touches it? In Ghidra:

```
References to 0x00394378 (5):
  0019996c  WRITE  from FUN_00199938
  00199e80  WRITE  from FUN_00199e30
  00199eec  WRITE  from FUN_00199e30
  00198c74  READ   from FUN_00198c60
  00198c90  READ   from FUN_00198c60
```

Two writers, one reader, and the reader is the spin loop from Module 2.7. **That is a complete diagnosis produced by five questions and no speculation:** something in the boot path must execute the store at `0x19996C` or `0x199E80`; until it does, `FUN_00198C60` spins forever.

Note the small surprise in that output, and keep the habit: the project's own notes described this flag as having "its only writer, `FUN_00199938`, unreachable dead code" — the binary says there are two writers. **The binary wins.** Documentation records what someone believed; cross-references record what the code does.

## 3.9 Practice 3 — say which region, and prove it

For each address: name the region, and give the one command that proves it.

```
0x00100008   0x01FFF000   0x004E0000   0x11008000   0x1FC00000   0xA0001234
```

**Checkpoint:**

| address | region | proof |
|---|---|---|
| `0x00100008` | ELF code, entry point | `inspect.py --va` → file offset `0x88`, first instruction |
| `0x01FFF000` | RDRAM stack (top of 32 MB), outside the ELF | `inspect.py` → "not inside any PT_LOAD" |
| `0x004E0000` | `.bss` | `inspect.py --va` → "past p_filesz (zero-initialised)" |
| `0x11008000` | VU1 microprogram memory | `inspect.py` → not PT_LOAD; `PS2_VU1_CODE_BASE` in `ps2_memory.h` |
| `0x1FC00000` | BIOS ROM | `PS2_BIOS_BASE` |
| `0xA0001234` | KSEG1 uncached → physical `0x00001234` | strip bit 31 (`va & 0x7FFFFFFF`) |

Notice the last one: after stripping, it lands in RDRAM. Both statements are true at once — "it is a hardware-window address" and "it is a RAM address". That is why the procedure asks about the prefix *before* declaring victory.

## 3.10 Checkpoint summary for Module 3

You are done with Module 3 when you can:

- convert a VA to a file offset without a tool, and know when the conversion is undefined;
- explain the difference between `filesz` and `memsz` using this ELF's numbers;
- say why `0x00001234`, `0x80001234` and `0xA0001234` might be the same byte and might not;
- run the five questions on an address you have never seen and reach an answer you can verify with a second tool.

---

# Module 4 — PS2 system software

## 4.1 Concept: four layers, and the test that separates them

Almost every "it doesn't work" in a port is a layer mistake. Learn to name the layer before you name the bug.

| Layer | What it is | Who wrote it | Where it lives | Can a recompiler translate it? |
|---|---|---|---|---|
| **Hardware** | The silicon: EE, GS, VU, SPU2, DMAC, INTC | Sony | — | No. Must be re-implemented. |
| **BIOS** | 4 MB ROM at `0x1FC00000`; boot, TLB setup, the kernel entry points | Sony | ROM | No. Must be replaced by host code. |
| **Kernel** | Threads, semaphores, timers, interrupts, exceptions — reached by `syscall` | Sony, inside the BIOS | ROM | No. Must be implemented in the runtime. |
| **Libraries (SDK)** | `libgraph`, `libpad`, `libcdvd`, `libmc`, the C runtime | Sony / Metrowerks | **linked into the ELF** | Yes — they are just code. But stubbing them is usually better than translating them. |
| **Middleware** | CRI `ADX`/`Sofdec`/`cvFs`, KORG's driver | Third parties | part in the ELF, part as `.IRX` | The EE part yes; the IOP part no. |
| **Game** | Namco's code | Namco | ELF | Yes, and this is the part you want translated. |

The **test** that separates them is mechanical:

> *Does the instruction stream contain the behaviour?*

- If yes → it is code, somewhere in the ELF, and it can be translated. The only question is whether it is *yours* to translate or a library you would rather replace.
- If no → it is a `syscall`, a hardware register write, or an IOP request, and the behaviour must be **implemented**, not translated.

Apply it: `WaitSema` at `0x00114720` is four instructions — `li v1,0x44`, `syscall`, `jr ra`, `nop`. The behaviour (block the thread, decrement the semaphore, reschedule) is not in those four instructions. Therefore it is **kernel**, and the port must supply it. Compare `FUN_00199938`, which is 20 instructions of `lui`/`addiu`/`sd`: the behaviour is entirely in the instruction stream, so it is **game code** and the recompiler owns it.

## 4.2 Concept: the syscall ABI, concretely

```
$v1 = syscall number
$a0..$a3 = arguments 0..3
$v0 = return value (negative = error)
syscall instruction traps to the kernel
```

Real examples from this game, all at addresses you can visit in Ghidra:

| Address | Code | Syscall |
|---|---|---|
| `0x00114720` | `li v1,0x44; syscall; jr ra; nop` | `WaitSema(0x44)` |
| `0x00114700` | same shape, `v1=0x42` | `SignalSema(0x42)` |
| `0x001144E0` | same shape, `v1=0x20` | `CreateThread(0x20)` |
| `0x00114500` | same shape, `v1=0x22` | `StartThread(0x22)` |

These stubs are why `data/config.toml` can list them as *stubs* with a name:

```toml
stubs = [
  "WaitSema@0x00114720",
  "SignalSema@0x00114700",
  "CreateThread@0x001144E0",
  "StartThread@0x00114500",
  ...
]
```

**What that TOML line means, precisely:** *do not translate the four instructions at `0x00114720`; instead make this address call the runtime's `WaitSema` handler.* It is the port declaring "this behaviour is kernel, not game". 55 such bindings exist in `data/config.toml`; 11,604 functions are translated. That ratio is typical, and it tells you where the work is.

## 4.3 Concept: threads and semaphores are how this game breathes

The EE runs a cooperative microkernel: many threads, one CPU, switch only at yield points (`WaitSema`, `SleepThread`, `ChangeThreadPriority`, `ExitThread`, VSync waits). There is no preemption inside a thread unless the kernel is entered.

That gives you a reading strategy: **every `WaitSema` is a place the game can legitimately stop**, and a port that hangs at a `WaitSema` is usually waiting for a thread that never got created, or a signal that never came.

You already have a real case study. `FUN_00199E30` (Module 2.4) does, in order:

```
ld  v0, 0(v1)               ; read the "done" flag at 0x00394380
bne v0, zero, bail          ; if already done, leave
sd  s1, 0(s2)               ; set 0x00394378 = 1  (the flag FUN_00198C60 spins on)
li  a1, 1 ; jal 0x00114570  ; ChangeThreadPriority(thread, 1)
lw  a0, 0(s0) ; jal 0x00114610 ; WakeupThread(thread)
```

So the sequence is: publish a flag, raise a thread's priority, wake it. The reader in `FUN_00198C60` sees the flag, stops spinning, sets the next flag, and kills itself. Classic hand-off.

**Now the port's version of that story, honestly:** in this project the main thread was observed going `Dormant` at `pc=1`, and one historical blocker was exactly this hand-off never completing (see `PS2_PROJECT_STATE.md`, and the archived state doc for the `0x394378` investigation). The mechanism is always the same and it is always one of these four:

1. the producer thread was never created,
2. the producer ran but took a branch that skipped the store,
3. the store happened to the wrong address (translation or delay-slot bug),
4. the consumer's `WaitSema` was implemented such that the wake never became a runnable thread.

Note that (4) is a *runtime* bug and (2) is a *game-logic* observation and (1) is usually a *translator* bug. Same symptom, three different owners. That is why Module 4 exists.

## 4.4 Concept: the IOP side, and where `.IRX` fits

The IOP runs its own kernel (in `IOPRP300.IMG` here — a bundle of boot modules: `File_Manager`, `Module_Manager`, `Timer_Manager`, `ROM_file_driver`), plus 16 game-supplied `.IRX` modules. This game's set is unusual and documented in `docs/04-executable.md`:

- `KORGIVAG.IRX` — KORG's IOP sound driver (2005), a third-party replacement for Sony's
- `IXAUDIO.IRX` — `IxAudio` 1.1
- `LIBSD.IRX`, `MCMAN.IRX`, `MCSERV.IRX` — Sony's sound library, memory card
- `PAD2/DS2U_D.IRX`, `SIO2MAN.IRX` — pad
- `INET/SMAP/NETCNF/DEV9` — the network stack (263,645 bytes of it, in a single-player RPG)
- `KORGUMDI.IRX` — a USB-MIDI driver naming six studio interfaces

The EE talks to these by **SIF** — two DMA channels (`SIF0`, `SIF1`) plus a command register — and by **RPC**: a request/response protocol where a client stub on the EE packs arguments, a server thread on the IOP unpacks them, and a semaphore signals completion.

The porting consequence: `.IRX` modules are **R3000A code**, a different CPU, and PS2Recomp does not translate them. The runtime *interprets* them (`ps2xIOP`, an R3000A interpreter plus a virtual IOP kernel) and bridges SIF/RPC to the EE side. So:

- "the game cannot find a file" → IOP file system + RPC, not EE translation;
- "music is silent" → KORG driver RPC + SPU2, not EE translation;
- "the pad does nothing" → `DS2U_D.IRX` + SIO2, not EE translation.

And the corollary that saves you the most time: **when the port stalls at a `WaitSema` whose peer is an RPC callback, the fix is in the RPC/SIF layer, and no amount of reading generated C++ will show it to you.**

## 4.5 Concept: what "the BIOS" is and is not

Three things get called "the BIOS" and they are different:

1. **The ROM** at `0x1FC00000` — 4 MB, contains the boot code and the kernel. Not present on a PC; the runtime replaces the entry path and never executes it.
2. **The kernel services the game calls** — the syscall table. These must be *implemented*, because the ROM is absent.
3. **The SDK libraries linked into the game** — `PsIIlibkernl3000`, `PsIIlibgraph3000`, `PsIIlibpad2 3000` and friends are *in the ELF* (their version strings are visible in the executable). These are ordinary translated code, but many of them end in a `syscall` and so delegate to (2).

Knowing which of the three you are looking at tells you whether the fix is "translate it" (3), "implement it" (2), or "it never runs on the port" (1).

## 4.6 Practice 4 — name the layer

For each item, say which layer owns it and what the port does with it:

1. `li v1,0x44; syscall`
2. the six `sd` stores in `FUN_00199938`
3. a write to `0x12000000`
4. a `jal` into `ADXT_Create` (CRI's ADX library, linked into the ELF)
5. `KORGIVAG.IRX` sending an RPC reply over SIF1
6. `addiu sp,sp,-0x40`

**Checkpoint:**

| item | layer | port treatment |
|---|---|---|
| 1 | kernel | implemented in `src/lib/Kernel/Syscalls/` |
| 2 | game | translated (it is in `data/output/`) |
| 3 | hardware | GS register — consumed by the GS model |
| 4 | library, in-ELF | translated, or stubbed if the runtime has a handler |
| 5 | IOP | interpreted as R3000A code, bridged by SIF/RPC |
| 6 | game | translated, one instruction at a time |

Six for six means you can now place any new symptom in the right box. That is the outcome Module 4 promises.

---

# Independent challenge — "prove it with a second tool"

**Requirements.** Produce a table for these nine addresses with four columns: *region*, *in the file? (yes/no/partial)*, *the command that proves it*, *one sentence on what it is*. Then answer the two questions below.

```
0x00100008
0x00199E30
0x00394378
0x004E9B80
0x01FFF000
0x1000F000
0x11008000
0x70000000
0xA0001234
```

**Questions.**

- **Q1.** `0x004E9B80` is the *end* of the load span. What is at that exact address, and why can you not read a byte there?
- **Q2.** `0x00394378` has two writers and one reader. Without running anything, state the condition under which the reader stops spinning, and name the instruction that satisfies it.

**Success criteria.** Every row is backed by a command you actually ran; the two questions are answered with evidence (an offset, an instruction address), not adjectives; and you can name one row where a single tool gives a misleading answer.

**Hints (one at a time, only if stuck).**

1. `inspect.py --va` answers three of the four columns by itself.
2. The end of a segment is exclusive: `vaddr + memsz` is the first byte *not* in it.
3. The reader is the spin loop; look at the `bne` that guards the producer's store.

<details>
<summary><strong>Solution (open only after you have attempted it)</strong></summary>

| address | region | in the file? | proof | what it is |
|---|---|---|---|---|
| `0x00100008` | ELF code | yes | `inspect.py --va 0x00100008` → offset `0x88` | entry point; first instruction is `padduw $at,$0,$0` (`clear at`) |
| `0x00199E30` | ELF code | yes | same; offset `= 0x80 + 0x99E30` | a thread hand-off function: read flag, store flag, change priority, wake thread |
| `0x00394378` | ELF data (`.data`) | yes, and the file says `00` | `inspect.py --va ... --dump 0x20` → offset `0x2943F8` | 64-bit "subsystem ready" flag, written by `FUN_00199938` and `FUN_00199E30` |
| `0x004E9B80` | end of the load span | — | `--segments` → `vaddr+memsz` | exclusive end: the first byte *not* mapped |
| `0x01FFF000` | RDRAM stack | no | `inspect.py` → "not inside any PT_LOAD" | top of the 32 MB, main thread's stack |
| `0x1000F000` | EE register (INTC) | no | `ps2_memory.h` `PS2_IO_BASE` + offset | interrupt controller status/mask |
| `0x11008000` | VU1 microprogram memory | no | `PS2_VU1_CODE_BASE` | where VU1's 16 KB of microcode is uploaded |
| `0x70000000` | scratchpad | no | `PS2_SCRATCHPAD_BASE` | 16 KB of EE-internal RAM, DMA cannot reach it |
| `0xA0001234` | KSEG1, uncached | no | strip bit 31 → `0x00001234` | same byte as `0x00001234`, read without the cache |

**Q1.** The load span is `[0x00100000, 0x004E9B80)`. The end address is exclusive, so `0x004E9B80` is the first byte *outside* the program. Reading it is an out-of-range access; `inspect.py` reports "not inside any PT_LOAD segment". (In practice, if the second `PT_LOAD` had content, that address would be its start — here it is empty, which is itself the answer.)

**Q2.** The reader `FUN_00198C60` executes `while (DAT_00394378 == 0) FUN_00198C48();`. It stops when a store makes `*(u64*)0x00394378` non-zero. The instructions that can do it are the `sd` at `0x0019996c` (`FUN_00199938`) and the `sd` at `0x00199e80` (`FUN_00199E30`, guarded by `bne v0,zero,0x00199eb8` where `v0 = *(u64*)0x00394380`). Either store satisfies the condition; **whichever one the boot path reaches first is the one that matters**, and that is a reachability question, not a data question.

**Where a single tool misleads:** `capstone`-based `tools/disasm_guest.py` prints *nothing at all* for `0x00100008`, which reads as "there is no code there". There is code; the disassembler simply refuses to decode MMI. Likewise Ghidra's decompiler renders the six stores of `FUN_00199938` as six separate globals and hides that they are a 16-byte-stride table. Neither tool lies; both omit.

</details>

---

# Troubleshooting

| Symptom | Likely cause | Verified recovery |
|---|---|---|
| `inspect.py` prints nothing for an address you can see in Ghidra | the address is not in this ELF (VU, GS, scratchpad, heap, or a mirror) | run `--segments`; if absent, use the map in 3.1 or strip the KSEG prefix |
| Disassembly stops after one line and shows `.word` | an MMI opcode the tool does not decode | use Ghidra, or `tools/old/dismips.py`; decode by hand with the bit fields in 2.2 |
| An address computed by hand is off by exactly `0x10000` | `addiu` sign-extended a low half `>= 0x8000` | add 1 to the `lui` immediate (2.4) |
| A branch appears to execute one instruction late | the delay slot | the underscored instruction runs *before* the jump; check the translator's ordering |
| A translated function returns one instruction too early, or the instruction after a `jal` runs twice | `$ra` computed as `PC+4` instead of `PC+8`, or the delay slot re-executed on return | `$ra = PC+8`; the slot runs before the transfer and must not run again when the callee returns |
| A side effect happens on the *not-taken* path of a branch | `beql`/`bnel` translated as `beq`/`bne` | the `*l` forms nullify the delay slot when not taken (2.5) |
| A `.bss` address shows garbage in hex, zeros in the file | `.bss` is not in the file | `p_filesz` vs `p_memsz` (Setup, 3.3) |
| A flag is read as 0 forever | no writer executed, wrong width, or wrong address | Ghidra xrefs on the address; check `sd` vs `sw`; check the delay slot |
| Two tools disagree about the bytes of an instruction | one prints bytes in memory order, the other prints the numeric word | compare like with like: `28 0C 00 70` ≡ `0x70000C28` (2.8) |
| Ghidra shows `DAT_00394378` where you expected a struct | the decompiler does not know it is a table | 6 flags at `+0x10` stride is a table (1.2) |

---

# Validation evidence

**Environment:** Windows, PowerShell; Python 3.15.0; Ghidra instance `:8192` with project `ps2` / `SLUS_212.01`; repository at `D:\Personal\C++\tales-of-legendia-ps2recomp`.

**Executed during this session (commands whose output is quoted above):**

| Check | Result |
|---|---|
| `python tools\old\ps2elf.py data\SLUS_212.01 --header` | header printed: entry `0x00100008`, 2 phdrs, 8 shdrs, span `0x00100000..0x004E9B80` |
| `python tools\old\ps2elf.py data\SLUS_212.01 --segments` | `filesz 0x002DAF80`, `memsz 0x003E9B80`, second phdr empty |
| `python docs\tutorial\sandbox\inspect.py … --header/--segments/--va` | all four modes produce the outputs quoted; `.bss`, non-PT_LOAD and file-backed cases each exercised |
| `python tools\old\dismips.py … --va 0x00199E30 12` | real disassembly, matches Ghidra's rendering including delay-slot markers |
| `python tools\disasm_guest.py … 0x100008 --count 4` | produced **no output** — confirmed limitation at MMI `padduw` |
| Ghidra `disassemble 0x00100008` | 132 instructions, first 28 are `clear` of GPRs |
| Ghidra `disassemble 0x00114720` | `li v1,0x44; syscall 0x0; jr ra; nop` |
| Ghidra `disassemble 0x0010012c`–`0x0010014c` | a `do/while` closed by `j 0x0010012c`, exited by `beq` |
| Ghidra `disassemble 0x00101610` | `jalr v1` with `$gp` saved/restored around it and the argument in the delay slot |
| Ghidra `disassemble 0x00100448` | `bnel` + skipped delay slot, then the compiler's compensating duplicate `lw` |
| Ghidra `disassemble 0x001004f8` | `beql` whose delay slot is the only store on the taken path |
| linear opcode sweep of the file-backed segment | `jal` most frequent transfer; `j`, `beql`, `bnel` present; `bltzal`/`bgezal` matches fall **outside** every Ghidra function, i.e. data |
| Ghidra `decompile 0x00199938` / `0x00198C60` | six-store flag setter; spin-then-`ExitDeleteThread` reader |
| Ghidra `xrefs 0x00394378` | 2 writers (`0x19996c`, `0x199e80`, `0x199eec`), 2 reads (`0x198c74`, `0x198c90`) |
| `python -c` on `data/SLUS_212.01[0x2DB02D:…]` | `.comment = MW MIPS C Compiler (2.4.1.01)\0PlayStation2\0` |
| `python -c` on `.reginfo` | `gprmask 0xF7FFFFFE`, `cprmask[1] 0xFFFFFFFF`, `gp = 0x003E2BF0` |
| `data/output/FUN_00199938_0x199938.cpp` inspected | generated C++ confirms `lui 0x39` → `57<<16` and `sd` at absolute `0x394378` |

**Static-only claims (read, not executed):** the timing and block descriptions in 1.1 (CPU/GS/SPU2 clocks, memory sizes) are from the platform documentation held in `.opencode/skills/ps2-recomp/resources/pdfs/` and the constants in `ps2_memory.h`; they were not measured on hardware or in an emulator during this session. The IOP module list in 4.4 is quoted from `docs/04-executable.md`, which documents `SLPS_255.33` (the Japanese build) — your `SLUS_212.01` may differ in module set, so treat the list as "what to look for", not as a manifest of your disc.

**Not tested:** any behaviour under PCSX2 or under the port itself; VU, GS, SPU2 and DMA register semantics beyond the addresses given; anything requiring a running IOP. The tutorial teaches how to find those out; it does not claim to have.

**Audience pilot:** none. Nobody has followed this document end to end, so the pacing and the time estimate are unmeasured.

**Known limitation to repeat honestly:** `docs/ps2-static-recompilation-overview.md` says the `0x394378` flag had "its only writer, `FUN_00199938`, unreachable dead code", while the cross-references show two writers. Both statements can be reconciled (one writer may be unreachable from the boot path), but the discrepancy is left visible on purpose: **the binary is the primary source and your notes are not.**

---

# Next steps

**You have now demonstrated** that you can go from a bare address to a subsystem, a byte, an instruction and a layer, on your own binary, with two tools that can be made to agree — which is the entire prerequisite for Part 2.

**Part 2 — The recompiler** will take these foundations and show:

1. how the analyzer decides what a function *is* (recompile / stub / skip / library-like), and why `data/config.toml` has 55 stubs for 11,604 functions;
2. how `jal` becomes a call to `dispatchGuestBranch` and why the flat table `g_ps2RecompiledFunctionTable` is indexed by `pc >> 2`;
3. the `switch (ctx->pc)` prologue that makes mid-function resume possible — and the merged-row defect that currently forces per-address overrides in `src/runtime/tol_overrides.cpp`;
4. how to read a generated `.cpp` next to its MIPS original and tell whether a difference is a translation bug or an environment gap.

**Part 3 — the environment layer**: syscalls, stubs, SIF/RPC, the IOP interpreter, and the four fix tools (TOML, runtime C++, game override, recompiler).

**Part 4 — graphics and debugging**: GIF packets, GS state, VU microcode, frame capture, and PCSX2-vs-port differential debugging.

**Primary sources actually consulted for this document**

- `data/SLUS_212.01` — the binary itself: ELF header, program headers, `.comment`, `.reginfo`, instruction bytes.
- `docs/04-executable.md`, `docs/01-overview.md` — disc, ELF and module inventory for Legends (JP build).
- `docs/ps2-static-recompilation-overview.md` — pipeline and runtime architecture of this repo.
- `PS2_PROJECT_STATE.md` — current blocker and the history of the `0x394378` investigation.
- `thirdparty/PS2Recomp/ps2xRuntime/include/runtime/ps2_memory.h` and `ps2_address.h` — authoritative address constants.
- `data/config.toml` — stub bindings and the `ghidra_export` counts.
- `.opencode/skills/ps2-recomp/resources/db-syscalls.md` — syscall numbers (`0x44` = `WaitSema`).
- Ghidra instance `:8192` — all decompiles, disassembly and cross-references quoted.

**Further reading available in this repository (not read in full for Part 1):** the EE Core Users Manual, EE Overview, VU Users Manual, GS Users Manual and `eeprogramming.pdf` under `.opencode/skills/ps2-recomp/resources/pdfs/`, plus `resources/01-ps2-hardware-bible.md` and `resources/02-mips-r5900-isa.md`.

---

# Appendix A — the five things readers ask about first

Reference material, not new teaching. Every number here was computed from `data/SLUS_212.01` in this session.

## A.1 Why every register is written with a `$`

`$` is an **assembler convention, not hardware**. It exists so a register name can never be confused with a label or a symbol: `a0` could be a function; `$a0` is unambiguously register 4. The number form is accepted everywhere — `$4` *is* `$a0`:

| Written | Also written | Number | Meaning |
|---|---|---|---|
| `$a0` | `$4` | 4 | first argument |
| `$sp` | `$29` | 29 | stack pointer |
| `$ra` | `$31` | 31 | return address |
| `$s8` | `$fp` | 30 | frame pointer (same register, two names) |
| `$f12` | — | 12 | FPU register (COP1), a separate file |
| `$vf01` | — | 1 | VU vector register (COP2), 128-bit |

You will see the `$` dropped constantly: Ghidra prints `addiu sp, sp, -0x40`, `dismips.py` prints `addiu sp, sp, -64`, and the Metrowerks comment in the generated C++ reads `lui $v1, 0x39`. Same instruction. The `$` never carries meaning you need; the **number** does, because raw decoder output only ever gives you numbers.

## A.2 MMI, `PADDUW`, and why "PADDUM" does not exist

**MMI** = Multimedia Instructions, Sony's extension, selected by opcode `0x1C`. There is no instruction called `PADDUM`. The two names it is probably being confused with are both real and both useful:

| Instruction | Class | What it does |
|---|---|---|
| `PADDUW` | parallel add, wrapping | four independent 32-bit unsigned adds across the 128-bit register; **no** carry between lanes; wraps on overflow |
| `PADDSW` | parallel add, saturating | same lanes, but clamps to `0x7FFFFFFF` / `0x80000000` instead of wrapping |
| `PMADDUW` | parallel multiply-add | 32×32→64 multiply-accumulate into HI/LO; the workhorse for fixed-point vertex maths |

`PADDUW` is the one in your entry point. Verified encoding, byte for byte:

```
bytes 28 0C 00 70  ->  word 0x70000C28
opcode 0x1C (MMI), rs=$zero, rt=$zero, rd=$at, bits10..6=0b10000, funct 0x28 (PADDUW)
=> padduw $at, $zero, $zero        Ghidra's pseudo-instruction: "clear at"
```

Why the compiler chooses this to zero a register: the R5900's GPRs are **128 bits**, and `padduw` touches all four lanes, so one instruction reliably clears all 16 bytes. Alternatives exist and you will meet them:

| Idiom | Clears | Note |
|---|---|---|
| `padduw $r,$zero,$zero` | all 128 bits | what Metrowerks emits here |
| `daddu $r,$zero,$zero` | low 64 bits | ordinary 64-bit add |
| `sq $zero, off(base)` | 16 bytes of *memory* | used for buffers, not registers |

The MMI families you will actually encounter, in rough order of frequency:

- **parallel arithmetic** — `PADDB/H/W`, `PADDUW`, `PADDSW`, `PSUBB/H/W`, `PSUBUW`, `PSUBSW`
- **pack / unpack** — `PCPYLD`, `PCPYUD`, `PEXTLW`, `PEXTUW`, `PEXTLB`, `PEXTUB` (the lane shuffles that make SIMD code readable)
- **multiply** — `PMADDW`, `PMADDUW`, `PMULTW`, `PMULTH`, `PMFHI`, `PMFLO`, `PHDIV`-adjacent forms
- **shift** — `PSLLH`, `PSLLW`, `PSRLH`, `PSRLW`, `PSRAH`, `PSRAW`
- **logic / compare** — `PAND`, `POR`, `PXOR`, `PNOR`, `PCGTW`, `PCEQW`, `PMAXW`, `PMINW`

**Porting consequence:** any translation that models a GPR as a scalar 64-bit `uint64_t` is defensible for ordinary code and *wrong* for MMI code, because the four lanes must not interact. This project's `R5900Context` stores GPRs as `__m128i` for exactly this reason.

## A.3 Delay slots — the complete rule set

1. The instruction **after** a branch or jump executes **before** the transfer takes effect.
2. This applies to `jal`, `jr`, `jalr` and `j` as well as `beq`/`bne`.
3. `jal` sets `$ra = PC + 8` — not `PC + 4` — because the PC has already advanced past the delay slot.
4. `beql` / `bnel` **nullify** the delay slot when the branch is *not* taken. So the instruction in the slot sometimes does not run at all. A translator that always executes it will corrupt state on the not-taken path.
5. Ghidra marks delay-slot instructions with a leading underscore (`_lw`, `_lui`). `dismips.py` does not mark them, which is why Ghidra is worth the startup time.
6. An interrupt cannot arrive between a branch and its delay slot.

The three shapes, from the real function at `0x00199E30`:

```
; call with a useful delay slot
00199e88  0C04515C   jal 0x00114570          ; jump to ChangeThreadPriority
00199e8c  0000048E   _lw a0, 0x0(s0)         ; ...but fetch the argument first

; branch with a useful delay slot
00199e60  14400015   bne v0, zero, 0x00199eb8 ; if flag set, leave
00199e64  3900153C   _lui s5, 0x39           ; ...but build this first

; plain return
00114728  03E00008   jr ra
0011472c  00000000   _nop
```

In generated code the delay slot must be emitted **before** the control transfer:

```cpp
// translated shape, not literal output
SET_GPR_S32(ctx, 4, runtime->Load32(rdram, ctx, GPR_U32(ctx, 16)));
ctx->pc = 0x199E90u;
dispatchGuestBranch(rdram, ctx, runtime, 0x00114570u, ...);
// and the *caller's* resume point is 0x199E90, not 0x199E8C
```

## A.4 "Offset" means five different things

The word appears in five unrelated senses. Before computing anything, ask: **offset from what base?**

| Sense | Example from this game | Rule | Answer |
|---|---|---|---|
| **1. load/store displacement** | `sd $ra, 0x38($sp)` | signed 16-bit; base is the register in parentheses | `sp + 56`; the frame is `0x40`, so this is the top slot |
| **2. branch displacement** | `0x14400015` = `bne v0,$zero,+0x15` | `(sign_extend(imm16) << 2) + (PC + 4)` | `0x199E64 + 0x54 = 0x00199EB8` |
| **3. jump target** | `0x0C04515C` (opcode 3 = `jal`) | `((PC+4) & 0xF0000000) \| (imm26 << 2)` | `0x00114570` |
| **4. `$gp` displacement** | `sw $a0, -0x7B44($gp)` | `$gp + sign_extend(imm16)`, `$gp = 0x003E2BF0` | `0x003DB0AC` |
| **5. file offset** | `--va 0x00394378` | `p_offset + (va - p_vaddr)` | `0x002943F8` |

Plus a sixth that is not an instruction at all:

| **6. structure field** | `lw $t9, 0x1C($a0)` | field at byte `0x1C` inside the object at `$a0` | vtable slot 7 (`0x1C / 4`) |

**Decision rule.** Read the parentheses first — the register inside names the base. No register → the value is PC-relative (branch, jump) or absolute (a `lui`/`addiu` pair). Not inside an instruction → you are looking at a file offset or a field offset, and the tool that produced it tells you which.

**Two traps.** (a) Branch displacements are **words**, not bytes: multiply by 4 before adding. (b) `addiu` sign-extends, so a `lui`/`addiu` pair whose low half is `>= 0x8000` needs the `lui` value raised by one (see 2.4).
