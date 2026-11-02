# Session 63 handover — Tales of Legendia native port

**Goal unchanged:** native PC port of *Tales of Legendia* (SLUS_212.01) via PS2Recomp. Runtime is a fixed asset from
`vamidi/PS2Recomp`; bend it to this game.

---

## 1. Where the port stands

| milestone | state |
|---|---|
| Recompile + launch | done — clang-cl + SDL3, 13,520 units, 0 decode failures, 0 dispatch errors |
| Guest reaches its own startup | done — `FUN_00165ad0 game-startup` runs |
| **Guest gets past heap init** | **done this session** (was the blocker) |
| Guest installs its syscall handlers | done — `FUN_0011e070` now returns; ~32 syscalls flow |
| Guest creates threads | still no — syscall `0x20` never reached |
| File I/O | none yet |
| Graphics | none — `transfers=0 draws=0 passes=0 presents=0` |

**Current blocker, stated precisely:** the guest reaches `FUN_00117ca0` = `sceSifInitCmd`, sends a 16-byte SIF
command to the IOP with `sceSifSetDma`, and spins on the guest flag `0x003DEF40` (`0x117DF8: jal 0x1174f8;
beq v0,zero,0x117DF8`). Nothing can set that flag: the runtime never delivers an IOP->EE SIF command
(`g_sifCmdHandlers` in `SIF.cpp` is only stored, never invoked), and the audio-only `ps2_native_iop` has no `sifcmd`.

## 2. What was fixed (two defects, in order)

### 2a. `InitMainThread` (syscall 0x3C) returned the bottom of the stack area

`crt0` at `0x1001C4` does `li v1,0x3c; syscall; move sp,v0` with `a1 = -1` and `a2 = 0x100000`.
`ps2_syscalls::SetupThread` returned `PS2_RAM_SIZE - 0x100000 = 0x01F00000` (the *bottom*) as `$sp` unless
`asyncCallbackStackTop() <= initialStack`, which is false in the normal case (pool top starts at `PS2_RAM_SIZE`).
Only `stack_size == 0` callers (DQ8) got `PS2_RAM_SIZE`.

With `sp = 0x01F00000` the main thread's frames sat inside the guest's own heap (`0x4E9B80..0x1F00000`), and the heap
init that follows immediately (`0x100230`: `memset(0x4E9B80, 0, 0x1A16480)`) zeroed them - including
`FUN_00100230`'s saved `$ra`. The thread then returned to pc 0 and went dormant, which is why nothing ever created a
thread, a timer or a draw.

### 2b. The Timer-2 handler ran on the guest stack

After 2a the guest's `$sp` was right but the first interrupt invocation got `$sp = 0x1FFFFF0` (the first pool stack,
carved from `PS2_RAM_SIZE`), because `reserveGuestStackFromAsyncPool(0x1F00000)` refuses when the guest stack bottom
equals the pool floor. Its frame zeroed the same saved `$ra` a second time.

Fix (both in `ps2xRuntime/src/lib/Kernel/Syscalls/System.cpp`, submodule):

- `$sp = PS2_RAM_SIZE` (the end of RDRAM, as the kernel does for `stack == -1`);
- the top `kInvocationStackReserve = 0x40000` (256 KiB = 16 x 16 KiB stacks) belongs to the invocation pool;
- the guest stack bottom handed to `setupCurrentThread()` is `max(initialStack, 0x1FC0000)`, so the pool is
  `[0x1F00000, 0x1FC0000)` and the guest stack is `[0x1FC0000, 0x2000000)` (256 KiB).

Verified: `[tick] #1 sp=0x01FBFFF0`, guest `sp=0x02000000`, heap-init frame survives, `FUN_0011e070` returns.

## 3. Assignment answers (session 62's list)

1. **Tick loop state block named.** `FUN_0011F650` is the EE **Timer 2** interrupt handler, registered by
   `AddIntcHandler(0xb, 0x11F650, 0, 0)` in `FUN_0011f0b0` at `0x11F150`. State = `0x00388CF8`
   (`s5 = 0x00390000` from `lui $s5,0x39` at `0x11F690`, minus `0x7308`); active list head = `0x00388D10`
   (state+0x18). **`$s5` is 0 on entry** - the handover's "log s5 on entry" prints 0, by construction.
2. **Queue dumped: always empty.** `count=0`, `free=0x003E3700` (untouched), `active=0`, `current=0xFFFFFFFF` on
   every tick, so startup never enqueues anything - the bug was upstream, exactly as the decision table said.
3. **The missing `CreateThread` is downstream of this.** The game loop (`0x1072B8`, `j 0x1072B8` at `0x100210`) is
   never reached because the main thread died in heap init. That is now fixed; the thread lives and the guest is
   inside libsif instead.
4. **Map holes** `0x232310`, `0x2324A8`, `0x2324BC` are still not hit (0 missing-function errors).

## 4. Next step, concretely

Give the SIF command channel an answer. Two routes:

- **Minimal:** complete the `sceSifInitCmd` handshake natively - the guest registers command handlers with
  `sceSifAddCmdHandler` (`FUN_00117828`, table at `0x3E07C0`) and waits on `0x3DEF40`; the packet it sends is 16
  bytes at `0x3DF000` with the command id in the payload. Answering means writing the reply into EE memory and
  invoking the registered handler, or simply setting the register/flag the guest polls.
- **Fuller:** wire up `ps2xIOP`'s LLE emulator (`IopSubsystem` + a ToL profile) instead of the audio-only
  `ps2_native_iop`; `builtin_profiles.cpp` shows the profile shape for other titles.

Then re-check `TOL_TRACE_SYSCALL` for `0x71` (`sceSifLoadModule`) and `0x20` (`CreateThread`), and `TOL_GFX_STATS_EVERY`
for the first draws.

## 5. Tooling

```
# build (runtime change + link, ~1 min)
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && ninja -C build/tol2 -j 12 tol'

# run from the repo root so cdRoot is data\CD
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

Probes (all env-gated, all in `src/runtime/tol_overrides.cpp` unless noted):

| env | what it prints |
|---|---|
| `TOL_TRACE_BOOT=1` | enter/exit of 14 boot points (`main.cpp`) |
| `TOL_TRACE_TICK=1` | tick-dispatcher state block + active list (new) |
| `TOL_TRACE_PC=0xADDR[,0xADDR]` | enter/exit of up to 16 functions with pc/ra/sp + 8 words at `$sp` (new) |
| `TOL_TRACE_PARK=1` | every `pc == 0` dormancy with the last 16 top-level dispatch pcs (`EeScheduler.cpp`) |
| `TOL_TRACE_SYSCALL=1` | first 150 syscalls with args, result, call site |
| `TOL_TRACE_DISPATCH=1` | first 4000 inter-function transfers |
| `TOL_TRACE_CKPT=1` | checkpoint-declined dispatches |
| `TOL_GFX_STATS_EVERY=N` | per-N-frame `[gfx]` counters |

Note: the guest's `syscall` instructions that the recompiler recognises as SDK stubs are emitted as **direct**
`ps2_stubs::*` calls, so they never appear in `TOL_TRACE_SYSCALL`. `sceSifSetDma` is one of them.

## 6. Land mines

- Never read a hardware address from memory: `0x10001000` is Timer 2, and `AddIntcHandler` cause `0xb` is Timer 2 too.
- `$s5`/`$gp` are not valid at entry to many guest functions - take addresses from the disassembly.
- A preempting checkpoint inside a *called* function unwinds the C++ stack and resumes as a fresh top-level dispatch
  of `ctx->pc`. That is only safe while the guest frame is intact, so anything that writes on the guest stack
  converts a preemption into a lost return address.
- `tol.exe` must not be running when linking.
- `thirdparty/PS2Recomp` carries local commits (now `9b82cef` + this session's fix); pushing still hangs on the network.
