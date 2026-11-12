# Part 0 — C, from zero, for reading translated code

*Prerequisite to Parts 1–4. Written for someone who has never written a line of C.*

> Read this **before** Part 2. Part 1 teaches you what the console does; this teaches you the language the recompiler writes its answers in. Every example here compiles and runs on your machine, and every output shown is output this machine actually produced.

---

## What you will be able to do after this chapter

1. **Compile and run** a C program on this machine with one command, and know where the `.obj`/`.exe` files went.
2. **Declare and reason about** the integer types the port lives on (`uint8_t` … `uint64_t`), including what happens when they overflow.
3. **Decode a real MIPS instruction word in C** — masks, shifts, and sign extension — and check the answer against Ghidra.
4. **Read and write pointers, arrays and structs**, including `uint8_t *rdram`, the one pointer this entire project is built around.
5. **Follow control flow** in a translated function, including `goto`, and explain why a recompiler emits `goto` at all.
6. **Read a macro call** and expand it by hand, which is how you read half of `SET_GPR_S32(...)`.
7. **Open any line** of `data/output/FUN_00199938_0x199938.cpp` and say exactly what it does to the guest CPU.

**Out of scope:** C++ classes, templates, exceptions, the standard library beyond `printf`, and anything about building the port itself (Part 3).

## Audience and prerequisites

No programming experience assumed. If you have written Python, JavaScript or a bit of C#, you will move faster but nothing here depends on it. Part 1 Module 2 (registers, instruction words) helps for section 0.4; everything else stands alone.

## Estimated time

**3–4 hours**, provisional — nobody has piloted this document. Sections 0.5 (pointers) and 0.12 (the challenge) are where most beginners need a second pass; budget for it.

## Setup

The compiler is **MSVC 14.44** (Visual Studio 18 Community), already installed. It is not on `PATH` by default, so each command starts a shell that knows where it is:

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && cl /nologo /W4 hello.c && hello.exe'
```

That `vcvars64.bat` prefix is the same one your project's own build notes use. In a normal terminal you would run it once and then type `cl hello.c`; in a fresh tool session we do it inline.

Everything lives in `docs/tutorial/sandbox/c/`, already written for you. To follow along from scratch, create that folder and put `hello.c` in it:

```c
#include <stdio.h>

int main(void)
{
    printf("hello, console\n");
    return 0;
}
```

Run the command above. **Expected output:**

```
hello, console
```

You now have `hello.obj` (the compiled object) and `hello.exe` (the linked program) next to `hello.c`. That two-step — compile, then link — is what `cl` hides from you.

**Cleanup:** `del docs\tutorial\sandbox\c\*.obj *.exe`. The `.c` files are worth keeping; they are the chapter's examples.

**No network, no installs, no credentials, no game files touched.**

---

## 0.1 The shape of a C program

Five things, and that is the whole grammar of a hello world:

```
#include <stdio.h>          bring in the declaration of printf
int main(void)              the function the operating system calls
{                           the body starts
    printf("hello\n");      a statement, ended by a semicolon
    return 0;               hand 0 back to the OS: success
}                           the body ends
```

Rules that will bite you in the first hour:

- **Every statement ends with `;`.** Not most — every one that is not a block or a preprocessor line.
- **Blocks are `{ ... }`.** Braces group statements. Indentation is for humans; C ignores it.
- **Case matters.** `Main` is not `main`. `Printf` is not `printf`.
- **Comments:** `/* ... */` and `// ... to end of line`. Comments are for you; the compiler deletes them.
- **`\n`** inside a string means "newline".

`int main(void)` means: a function named `main`, returning an `int`, taking no parameters (`void` means "nothing"). The OS calls it; `return 0` means "it worked".

## 0.2 Types: the ones this project uses

C makes you say what kind of number a variable holds, and the size is part of the type. The port does not use `int` for anything important — it uses **fixed-width** types from `<stdint.h>`, because a PS2 address must be exactly 32 bits and a register exactly 64, on every machine.

`docs/tutorial/sandbox/c/types.c` prints the sizes on this machine:

```c
printf("int         %2u bytes\n", (unsigned)sizeof(int));
printf("uint32_t    %2u bytes\n", (unsigned)sizeof(uint32_t));
```

**Verified output:**

```
char         1 bytes
int          4 bytes
long long    8 bytes
uint8_t      1 bytes
uint16_t     2 bytes
uint32_t     4 bytes
uint64_t     8 bytes
void *       8 bytes
addr        0x00394378
flag        1
```

What to take from that table:

| Type | Bytes | Use it for |
|---|---|---|
| `uint8_t` | 1 | one byte of guest RAM |
| `uint16_t` | 2 | halfword |
| `uint32_t` | 4 | **every PS2 address**, every 32-bit instruction, every `lw`/`sw` |
| `uint64_t` | 8 | `ld`/`sd` values, dual-issue pairs |
| `int32_t` | 4 | a signed value where negative matters (an `addiu` immediate) |
| `void *` | 8 | a host pointer — 8 bytes because the host is 64-bit |

`u` in `uint32_t` means **unsigned**: only zero and positives, range `0 … 4294967295`. `int32_t` is signed: `-2147483648 … 2147483647`.

**Why not just `int`?** Because the console's `int` is 32 bits and the host's is also 32 bits *today* — but `long` is 4 on Windows and 8 on Linux. Code that touches guest memory uses the fixed-width names so a byte stays a byte when the port is built elsewhere.

**Checkpoint.** Change `types.c` to also print `sizeof(uintptr_t)` and `sizeof(size_t)`. Expected: `8` and `8`. If you get something else, you are not compiling for 64-bit.

## 0.3 Arithmetic, wrapping, and the trap in `int`

Unsigned integers **wrap**: add 1 to the largest and you get 0. That is not an error, it is the definition, and the console relies on it.

`wrap.c`:

```c
uint32_t max = 0xFFFFFFFFu;
printf("0xFFFFFFFF + 1          = 0x%08X\n", max + 1u);
```

**Verified output:**

```
0xFFFFFFFF + 1          = 0x00000000
0x00100000 - 0x40       = 0x000FFFC0
-1 as uint32_t          = 0xFFFFFFFF
0xA0001234 & 0x01FFFFFF = 0x00001234
```

Three facts from that output, all of which you will use:

1. **`sp - 0x40` never goes negative** because `sp` is unsigned. Underflow wraps to a huge number — which is exactly why a bad frame size produces an address like `0xFFFFFFC0` instead of a crash. When you see that in a trace, suspect a subtraction on an unsigned value, not a memory fault.
2. **`-1` and `0xFFFFFFFF` are the same 32 bits.** The type tells the compiler whether to print it as `-1` or `4294967295`. This is why the port casts constantly.
3. **`& 0x01FFFFFF` folds an uncached address onto its cached twin** (Part 1 §3.2). That is the `PS2_RAM_MASK` from `ps2_memory.h` doing its job.

**The trap:** *signed* overflow is **undefined behaviour** in C. `int x = INT_MAX; x = x + 1;` is not guaranteed to wrap — the compiler is allowed to assume it never happens and optimise accordingly. Unsigned overflow **is** well defined.

That asymmetry is why the generated code never writes `a + b` for guest arithmetic. It writes:

```cpp
SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 3), 17272));
```

`ADD32` is a helper that does the addition on `uint32_t` (defined wrap) and casts the result. **The wrapping the console hardware does is unsigned wrapping, so the port must compute it unsigned.** If you ever find yourself "fixing" one of these to plain `+`, you will introduce a bug that only appears on overflow — usually months later, in a menu.

**Checkpoint.** In `wrap.c`, print `0x00100000u - 0x00200000u`. Expected: `0xFFF00000`. Then print the same expression assigned to an `int32_t`. Expected: `-1048576`. Same bits, different reading.

## 0.4 Bits: masks, shifts, and decoding a real instruction

Three operators do all the work:

| Operator | Meaning | Example |
|---|---|---|
| `&` | AND — keep only the bits set in both | `w & 0x3F` keeps the low 6 bits |
| `\|` | OR — set those bits | `0x80 \| 0x01` = `0x81` |
| `^` | XOR — flip those bits | `0xFF ^ 0x0F` = `0xF0` |
| `~` | NOT — flip every bit | `~0u` = `0xFFFFFFFF` |
| `<<` | shift left (multiply by 2ⁿ) | `1u << 4` = `0x10` |
| `>>` | shift right (divide by 2ⁿ) | `0x40u >> 4` = `4` |

To pull a bit-field out of a word: **shift it down, then mask off what is left**:

```c
uint32_t rs = (w >> 21) & 0x1Fu;   /* 5 bits starting at bit 21 */
```

`bits.c` does this to the first instruction of Tales of Legendia:

```c
uint32_t w = 0x70000C28u;
```

**Verified output:**

```
word    0x70000C28
opcode  0x1C  (28)
rs      $0
rt      $0
rd      $1
sa      16
funct   0x28  (40)  = PADDUW
0x27BDFFC0 immediate   0xFFC0 -> -64
```

You have now reproduced Part 1's hand decode with code, including the part I got wrong by hand the first time: `sa` is **16**, not 0, because MMI reuses that field as part of the opcode.

The last line is the other half of the lesson. An `addiu` immediate is **16 bits and signed**:

```c
uint32_t raw = 0x27BDFFC0u & 0xFFFFu;    /* 0xFFC0          */
int32_t  imm = (int32_t)(int16_t)raw;    /* sign-extend     */
```

`(int16_t)raw` says "read these 16 bits as a signed number", which turns `0xFFC0` into `-64`. Then `(int32_t)` widens it. **Without the first cast you get 65472, and every stack frame size you compute will be wrong.** This exact two-step cast appears all over the runtime's instruction helpers.

**Checkpoint.** Decode `0x27BDFFC0` fully in `bits.c` (opcode, rs, rt, immediate) and confirm against Ghidra: `addiu sp, sp, -0x40`. Expected: opcode `0x09`, rs `29`, rt `29`, imm `-64`.

## 0.5 Pointers: the one concept worth slowing down for

A pointer is **an address, plus a note about what is stored there**. Two operators:

- `&x` — "the address of x"
- `*p` — "the thing p points at" (dereference)

```c
uint32_t flag = 1;
uint32_t *p = &flag;    /* p holds the address of flag */
*p = 2;                 /* write through the pointer   */
printf("%u\n", flag);   /* prints 2                    */
```

The port's central object is exactly this:

```cpp
void FUN_00199938_0x199938(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime)
```

`rdram` is a pointer to the guest's 32 MB of RAM. `ctx` points at the CPU's registers. `runtime` points at the machine. Every translated function receives these three.

**Indexing is pointer arithmetic.** `p[i]` means `*(p + i)`, and the `+ i` is scaled by the size of what `p` points at:

- `uint8_t *p; p[0x10]` → address + 16 bytes.
- `uint32_t *p; p[4]` → address + 16 bytes (four 4-byte elements).

That is why `rdram` is `uint8_t *`: with a byte-sized element, `rdram[addr]` is *literally* "the byte at guest address `addr`", with no scaling to remember.

`pointers.c` builds a 1 KB fake RAM and implements a 32-bit store and load by hand, because that is what `Load32`/`Store32` ultimately do:

```c
static void store32(uint32_t addr, uint32_t value)
{
    rdram[addr + 0] = (uint8_t)(value & 0xFFu);
    rdram[addr + 1] = (uint8_t)((value >> 8) & 0xFFu);
    ...
}
```

**Verified output:**

```
four bytes    EF BE AD DE
load32       0xDEADBEEF
sizeof rdram 1024 bytes
after p[0x10]=0x42, load32 = 0xDEADBE42
```

Read that first line again: the value `0xDEADBEEF` is stored **little-endian**, so the byte at the lowest address is `EF`. This is Part 1's endianness lesson, now in code.

**Three pointer rules you must not violate.**

1. **Never dereference a pointer you did not validate.** Guest code passes addresses as integers; the runtime must check they are in range before using them. A bad guest pointer becomes a host crash at a place far from the bug.
2. **`*` in a declaration is not the same `*` as in an expression.** `uint32_t *p;` declares; `*p = 1;` dereferences.
3. **`NULL`/`0` is not a valid address to read.** We will prove that in 0.11.

**Checkpoint.** Modify `pointers.c` to `store32(0x14, 0x11223344u)`. Expected bytes at `0x14`: `44 33 22 11`. If you wrote them in the wrong order, you are thinking big-endian — the console is not.

## 0.6 Structs: teaching C what a CPU context looks like

A struct groups fields, and C lays them out **in declaration order**, aligned to each field's size. `structs.c` models the register file:

```c
typedef struct {
    uint8_t  gpr[32][16];   /* 32 GPRs x 16 bytes */
    uint32_t pc;
    uint32_t hi;
    uint32_t lo;
} Cpu;
```

**Verified output:**

```
sizeof(Cpu)        524
offsetof(pc)       512
offsetof(hi)       516
offsetof(lo)       520
cpu.pc            0x00100008
p->pc             0x00100008
(*p).pc           0x00100008
```

- `sizeof` = 524 bytes: `32 × 16 = 512`, then three 4-byte fields.
- `offsetof(Cpu, pc)` = 512: the byte offset of `pc` inside the struct. This is how you reason about a decompiler's `*(int *)(param_1 + 0x200)`.
- `.` accesses a struct variable; `->` accesses through a *pointer to* a struct. `p->pc` and `(*p).pc` are the same thing; `->` is just easier to read.

Two things that matter in this project:

1. **`gpr` is `[32][16]` because R5900 registers are 128 bits** (Part 1 §2.1). The real `R5900Context` uses `__m128i gpr[32]` — 16 bytes each — for the same reason.
2. **Field order is behaviour.** The guest's structs live in guest RAM and the game indexes them by hard-coded offsets (`+0x1C`, `+0x200`). If you write a host struct to overlay guest memory and reorder the fields, everything breaks. When in doubt, read out of `rdram` at an explicit offset instead of casting a struct over it.

**Checkpoint.** Add `uint32_t lo1;` after `lo` and print `sizeof(Cpu)` again. Expected: `528`. Then print `offsetof(Cpu, lo1)`. Expected: `524`.

## 0.7 Functions

```c
uint32_t load32(uint32_t addr);                  /* declaration: "this exists" */
uint32_t load32(uint32_t addr) { return 0; }     /* definition:  "here it is"   */
```

- Parameters are **passed by value**: the function gets a copy. That is why a function that must modify the caller's data takes a **pointer** — `void store32(uint8_t *mem, uint32_t addr, uint32_t value)`.
- `void` as a return type means "returns nothing".
- `static` on a function means "visible only in this file". Most helpers in this project are `static`.

The translated functions are a fixed shape, and it never varies:

```cpp
void name(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);
```

Three parameters, no return value. **Results come back through `ctx`**, because a guest function's result is its `$v0` register, not a C return value. When you see `setReturnU32(...)` in the runtime, that is the helper that writes the guest's return register for a stub.

## 0.8 Control flow, and why generated code is full of `goto`

Ordinary branches:

```c
if (x == 0) { ... } else { ... }
while (x != 0) { ... }
for (int i = 0; i < 6; i++) { ... }
switch (op) { case 0x20: ...; break; default: ...; }
```

And `goto`, which most tutorials tell you never to use, and which this project uses deliberately.

`control.c` shows the reason:

```c
again:
    steps++;
    printf("  execute pc=0x%08X\n", pc);
    if (pc == 0x00100008u) { pc = 0x0010000Cu; goto again; }
```

**Verified output:**

```
resume at the entry point:
  execute pc=0x00100008
  execute pc=0x0010000C
  execute pc=0x00100010
  stop after 3 instruction(s)
resume in the middle (as dispatchGuestBranch can):
  execute pc=0x00100010
  stop after 1 instruction(s)
```

The second block is the important one. A guest PC can be **any instruction address**, including the middle of a function — because of function pointers, vtables, and RPC callbacks. A C `while` loop cannot start in the middle; a label plus `goto` can.

That is why every generated file begins with a resumption table:

```cpp
switch (ctx->pc) { case 0x10012cu: goto label_10012c; ... default: break; }
```

Read it as: *"if the guest is resuming at one of my internal addresses, jump straight there; otherwise start at the top."*

**Checkpoint.** In `control.c`, add a third `run(0x0010000Cu)` call. Expected: it executes two instructions (`0x10000C`, then `0x100010`) and stops. If it executes three, your chain of `if`s starts one address too early.

## 0.9 Macros: `SET_GPR_S32` explained

A macro is **text substitution before compiling**:

```c
#define SET_GPR_S32(ctx, reg, value) \
    do { (ctx)->gpr[(reg)] = (uint32_t)(value); } while (0)
```

`macro.c` uses it to reconstruct the two instructions that build `0x00394378`:

```c
SET_GPR_S32(&ctx, 3, (int32_t)((uint32_t)57 << 16));   /* lui   $v1, 0x39      */
SET_GPR_S32(&ctx, 3, ctx.gpr[3] + 17272u);             /* addiu $v1, $v1, 0x4378 */
```

**Verified output:**

```
after lui        $v1 = 0x00390000
after addiu      $v1 = 0x00394378
```

Four details that make macros safe, and that you will see again verbatim in the generated code:

1. **`do { ... } while (0)`** makes the macro a single statement, so `if (x) SET_GPR(...);` cannot accidentally capture the following line.
2. **Parenthesise every parameter** — `(ctx)`, `(reg)` — because substitution is textual: pass `a + 1` without parentheses and you get `->gpr[a + 1]` in one place and `->gpr[a + 1 * 2]` in another.
3. **`\` at end of line** continues the macro.
4. **No type checking.** A macro is not a function; wrong arguments produce strange errors at the *call* site.

`ADD32` and `GPR_U32` are macros or inline helpers of the same kind. When reading generated code, expand them mentally — that is the whole trick.

## 0.10 Worked example: reading a real generated file

Here is the top of `data/output/FUN_00199938_0x199938.cpp`, which is the translation of the function from Part 1 §1.2 (the six ready-flags):

```cpp
void FUN_00199938_0x199938(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
    ctx->pc = 0x199938u;

    // 0x199938: 0x3c030039  lui         $v1, 0x39
    ctx->pc = 0x199938u;
    SET_GPR_S32(ctx, 3, (int32_t)((uint32_t)57 << 16));

    // 0x199950: 0x24020001  addiu       $v0, $zero, 0x1
    ctx->pc = 0x199950u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 0), 1));

    // 0x199954: 0x24634378  addiu       $v1, $v1, 0x4378
    ctx->pc = 0x199954u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 3), 17272));

    // 0x19996c: 0xfc620000  sd          $v0, 0x0($v1)
    ctx->pc = 0x19996cu;
    do {
        uint64_t _value = static_cast<uint64_t>(GPR_U64(ctx, 2));
        ps2TraceGuestWrite(rdram, 0x394378u, 8u, _value, 0u, "WRITE64", ctx);
        FAST_WRITE64(0x394378u, _value);
    } while (0);
```

Line by line:

| Line | Read it as |
|---|---|
| `void FUN_...(...)` | the fixed translated-function signature; results go through `ctx` |
| `ctx->pc = 0x199938u;` | tell the runtime which guest instruction we are on (traceability) |
| `// 0x199938: 0x3c030039 lui...` | the original instruction and word, kept as a comment — **your ground truth** |
| `57 << 16` | `0x39 << 16` = `0x00390000`; the `u` suffix means unsigned |
| `(int32_t)` then `(uint32_t)` inside the macro | the classic fidelity cast: keep the bit pattern, satisfy the type |
| `ADD32(GPR_U32(ctx, 3), 17272)` | guest add with **defined** 32-bit wrapping, not host `+` |
| `do { ... } while (0)` | a macro expansion that happens to be written out longhand |
| `static_cast<uint64_t>(GPR_U64(ctx, 2))` | C++ cast syntax — generated files are `.cpp` |
| `ps2TraceGuestWrite(...)` | a diagnostic hook; the interesting argument is the **absolute** address `0x394378` |
| `FAST_WRITE64(0x394378u, _value)` | the actual guest store |

Two things worth noticing, because they are what makes this code **readable**:

- The original instruction is in the comment, so you can always check the translation against Part 1's method.
- The address appears twice, once symbolic (`$v1`, in the comment) and once resolved (`0x394378u`). The compiler folded `lui`+`addiu` into a constant. When the address is *not* constant, you will instead see the register arithmetic — that is the case that breaks.

**Checkpoint.** Open `data/output/FUN_00199938_0x199938.cpp` and find the `jr $ra` at the end. Expected: `ctx->pc = 0x199980u;` then a `GPR_U32(ctx, 31)` read. Explain in one sentence why the return address is in register 31 and not a C `return`.

## 0.11 Undefined behaviour: the compiler will not save you

`docs/tutorial/sandbox/c` has no example for this on purpose — I wrote one in a temp folder and ran it. It is four lines:

```c
volatile char *p;
p = 0;
*p = 1;
```

Compiled with `cl /W3`: **no error, no warning**. It builds and links. Run it:

```
about to write through a null pointer
exit code = -1073741819 (0xC0000005)
```

`0xC0000005` is an access violation — the host process died. The `printf("survived")` after it never ran. And the earlier version without `volatile` exited *successfully*, because the optimiser was entitled to delete a store to an address that cannot be dereferenced.

That is the lesson: **undefined behaviour is not "checked at start-up". It is a licence for the compiler to do anything, and it usually shows up as a crash somewhere else, later.**

The four that will actually happen in this project:

| UB | Looks like | Guard |
|---|---|---|
| null/invalid pointer | crash at `0xC0000005` | validate guest addresses before dereferencing |
| out-of-bounds index | silent corruption of a neighbour | bound-check `addr < size` |
| signed overflow | wrong values only at extremes | do guest maths in `uint32_t`, use `ADD32` |
| reading uninitialised memory | different answers per run | `= {0}` or `memset` first |

## 0.12 Practice — reimplement the six stores

Write `flags.c` (a started version is in the sandbox) that:

1. allocates a 4 MB `static uint8_t rdram[0x400000];`
2. implements `store64(addr, value)` and `load64(addr)` byte by byte, little-endian;
3. performs the six stores from `FUN_00199938` — first address `0x00394378`, stride `0x10`, six iterations, value `1`;
4. prints each address and then reads all six back.

**Checkpoint — expected output:**

```
before: 0
sd  1 -> 0x00394378
sd  1 -> 0x00394388
sd  1 -> 0x00394398
sd  1 -> 0x003943A8
sd  1 -> 0x003943B8
sd  1 -> 0x003943C8
after : 1
  [0x00394378] = 1
  [0x00394388] = 1
  [0x00394398] = 1
  [0x003943A8] = 1
  [0x003943B8] = 1
  [0x003943C8] = 1
```

If your addresses step by 8 instead of 16, you have written `stride = 8` — the guest stores 8 bytes but the *table* strides 16. If `load64` returns `4294967296`, you shifted by 32 instead of `8 * i`.

You have now written, in miniature, the part of the runtime that the whole port depends on: **a byte array becomes guest memory, and a struct becomes the CPU**.

## 0.13 Independent challenge — `region_of()`

**Requirements.** Write `region.c` with a function

```c
static const char *region_of(uint32_t addr);
```

that returns the name of the PS2 region for any address, per Part 1 §3.1, and a `main` that prints the region for these ten addresses:

```
0x00100008  0x00394378  0x004E0000  0x01FFF000  0x1000F000
0x11008000  0x12000000  0x1FC00000  0x70000000  0xA0001234
```

Rules: no external arrays of magic answers; the function must decide from the address alone. Use `uint32_t`, not `int`. Compile with `/W4` and produce no warnings.

**Success criteria.** Output matches the table below exactly; you can explain why `0xA0001234` is *not* reported as RDRAM even though it aliases it; and you can add `0x12345678` and justify the answer.

**Hints.**
1. Order the `if`s so the specific ranges come before the general ones.
2. `0x7xxxxxxx` and `0x1FCxxxxx` are not inside the first range — check them first only if you write the first test narrowly.
3. A range is `[base, base + size)` — the end is **exclusive**.

<details>
<summary><strong>Solution (open after attempting)</strong></summary>

```c
#include <stdio.h>
#include <stdint.h>

static const char *region_of(uint32_t a)
{
    if (a <  0x02000000u)                      return "RDRAM (ELF image / heap / stack)";
    if (a >= 0x10000000u && a < 0x10010000u)   return "EE registers (timers/DMA/INTC)";
    if (a >= 0x11000000u && a < 0x11010000u)   return "VU0/VU1 code+data memory";
    if (a >= 0x12000000u && a < 0x12002000u)   return "GS privileged registers";
    if (a >= 0x1FC00000u && a < 0x20000000u)   return "BIOS ROM";
    if (a >= 0x70000000u && a < 0x70004000u)   return "scratchpad (16 KB)";
    if (a >= 0x80000000u && a < 0xA0000000u)   return "KSEG0 (cached direct map)";
    if (a >= 0xA0000000u && a < 0xC0000000u)   return "KSEG1 (uncached direct map)";
    if (a >= 0xC0000000u)                      return "KSEG2 (TLB mapped)";
    return "unmapped";
}
```

Verified output of `region.c` (as shipped in the sandbox):

```
0x00100008  RDRAM (ELF image / heap / stack)
0x00394378  RDRAM (ELF image / heap / stack)
0x004E0000  RDRAM (ELF image / heap / stack)
0x01FFF000  RDRAM (ELF image / heap / stack)
0x1000F000  EE registers (timers/DMA/INTC)
0x11008000  VU0/VU1 code+data memory
0x12000000  GS privileged registers
0x1FC00000  BIOS ROM
0x70000000  scratchpad (16 KB)
0xA0001234  KSEG1 (uncached direct map)
```

**Why `0xA0001234` is not RDRAM:** the address space is checked in order. `0xA0001234` is above `0x02000000`, so the first test fails; it is inside `[0xA0000000, 0xC0000000)`, so it is reported as KSEG1. It *aliases* physical `0x00001234` — which is RDRAM — but the address you were given is a KSEG1 window onto it. Both statements are true; the function reports the window.

**`0x12345678`:** not below `0x02000000`, not in any of the listed ranges → `"unmapped"`. On real hardware that region is documented as unused/reserved; the honest answer is "no region claims it".

</details>

## 0.14 Troubleshooting — real compiler messages from this machine

| Message | Cause | Fix |
|---|---|---|
| `error C2143: syntax error: missing ';' before 'return'` | a statement was not terminated | add the `;` to the line **before** the one reported |
| `warning C4477: 'printf' : format string '%d' requires an argument of type 'int', but variadic argument 1 has type 'uint64_t'` | wrong format specifier | use `%llu` (with a cast) or `%I64u`; the compiler's `note:` lines name the valid ones |
| `warning C4101: 'unused': unreferenced local variable` | you declared something you never used | delete it, or use it — this warning is a reliable smell |
| compiles clean, exits `0xC0000005` | you dereferenced a bad pointer | validate the address; see 0.11 |
| compiles clean, prints nonsense | read uninitialised memory, or wrong format | initialise with `= {0}`; check `%u` vs `%d` |
| `error C2100: you cannot dereference an operand of type 'S'` | you wrote `*x` where `x` is not a pointer (often a struct instead of a struct pointer) | use `x.field`, or take the address with `&x` |
| `error LNK2019: unresolved external symbol helper referenced in function main` | you declared a function but never defined it | add the definition, or fix the name/case |
| value prints negative when it cannot be | the type is signed | use `uint32_t` and `%u` |

## 0.15 Validation evidence

**Environment:** Windows, MSVC 14.44.35207 via `vcvars64.bat` from Visual Studio 18 Community; commands run in `docs/tutorial/sandbox/c`.

**Compiled and executed in this session:**

| Program | Result |
|---|---|
| `hello.c` | `hello, console` |
| `types.c` | sizes: char 1, int 4, long long 8, uint8 1, uint16 2, uint32 4, uint64 8, void* 8 |
| `wrap.c` | `0x00000000`, `0x000FFFC0`, `0xFFFFFFFF`, `0x00001234` |
| `bits.c` | opcode `0x1C`, rs 0, rt 0, rd 1, **sa 16**, funct `0x28`; immediate `0xFFC0 → -64` |
| `pointers.c` | bytes `EF BE AD DE`; load `0xDEADBEEF`; `0xDEADBE42` after overwriting one byte |
| `structs.c` | `sizeof 524`, offsets 512/516/520; `.` `->` `(*p).` all equal |
| `control.c` | resumption at entry runs 3 steps; resumption mid-function runs 1 |
| `macro.c` | `0x00390000` then `0x00394378` |
| `flags.c` | `before: 0`, six stores, `after : 1` |
| `region.c` | the ten-line table in 0.13 |
| deliberate errors (temp folder) | `C2143`, `C4477` + `%lld` note, `C4101`, `C2100`, `LNK2019`/`LNK1120`, and a clean build that exits `0xC0000005` |

**Not tested:** other compilers (no gcc/clang on this machine), 32-bit builds, C++ compilation of these files, and anything involving the port itself. `int` being 4 bytes and `void *` 8 bytes is this machine's truth; the fixed-width types are what make the code portable.

**Pilot:** none. Pacing and the time estimate are unmeasured.

## 0.16 Next steps

**Demonstrated outcome:** you can compile C, read pointers and structs, decode a guest instruction in code, explain `goto`-based resumption, expand a macro, and recognise the four kinds of undefined behaviour that matter here.

**Where this plugs into the rest:**

| You learned | It appears in |
|---|---|
| `uint8_t *rdram` | every generated function; `Load*/Store*` in `PS2Runtime` |
| masks and shifts | instruction decoding (`r5900_decoder.cpp`), GS register packing |
| `static_cast` / casts | every translated instruction |
| `do { } while (0)` | `SET_GPR_S32`, `FAST_WRITE64`, `ps2TraceGuestWrite` |
| `goto` + labels | the `switch (ctx->pc)` resumption prologue |
| structs at fixed offsets | `R5900Context`, guest objects read at `+0x1C`, `+0x200` |
| wrapping arithmetic | `ADD32`, `SUB32`, and every MIPS arithmetic helper |

**Part 1** (already written) is the machine. **Part 2** is the recompiler, and it will assume this chapter's vocabulary: function table, dispatch, translated body, stubs, overrides.

**Primary sources consulted:** `data/output/FUN_00199938_0x199938.cpp` (quoted verbatim above), `thirdparty/PS2Recomp/ps2xRuntime/include/runtime/ps2_memory.h`, `PS2_PROJECT_STATE.md` for the build command, `docs/tutorial/part1-ps2-ecosystem.md` for the addresses and the MIPS decode; MSVC's own diagnostics, quoted as printed.
