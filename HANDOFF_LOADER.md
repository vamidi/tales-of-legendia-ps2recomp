# Handoff — Tales of Legendia native port: getting past `NOW LOADING`

**Read this first.** The long narrative history is in `PS2_PROJECT_STATE.md` (sessions 78–91).

## Status

The game boots, initialises CRI/ADXF, reads the AFS extent tables, runs the script dispatcher and the
GS worker — but the **boot script asset never lands**, so `NOW LOADING` never advances to the first logo.

Current stall (`TOL_TRACE_SPIN=1`):

```
[spin] pc=0x198C88 ra=0x198C90    (identical for 6,600,000+ branches)
```

`0x198C60` is a guest **thread entry** waiting on a flag another thread sets:

```c
while (DAT_00394378 == 0) FUN_00198c48();
DAT_00394380 = 1;
ExitDeleteThread();
```

On real hardware those flags are `0` at the same point too — the thread is *legitimately parked*. This
port runs thread entries inline, so parking is fatal. **That is the current class of blocker.**

---

## 0. What this project is — and the framing error behind sessions 84–91

This is a **source port by static recompilation**, not an emulator. The game's MIPS R5900 is translated
ahead of time into C++ (`data/output_fixed/`, 53,558 TUs) and compiled into a normal Windows
executable. There is **no CPU interpreter and no faithfulness loop**. Three layers, and only one of
them is ours:

| Layer | What it is | Rule |
|---|---|---|
| `data/output_fixed/*.cpp` | the game's own code, translated | machine output; do not hand-edit; a defect here is a **recompiler** bug |
| `ps2xRuntime` | the *outside world* the guest talks to: RAM/MMU, DMA, VIF/GIF, GS, SIF, IOP, CDVD, timers, VU | must behave like **hardware**, never fabricate results |
| `src/runtime`, `src/gfx`, `src/ui` | integration, GS rasteriser, menu | ours to change |

**What the job entails.** Supply only what lies outside the ELF: BIOS syscalls, IOP modules, the disc,
hardware registers, a GS rasteriser, input/audio, and the host window. Everything the guest implements
itself — its kernel, its thread scheduler, its coroutines, its file system, its loaders, its script
engine — is *compiled in* and must simply be allowed to run. The diagnostic question is always the
skill's: **"is the translation wrong, or is the environment incomplete?"**

**The framing errors that produced the current mess.** All four are the same mistake: treating guest
logic as something the host must emulate.

1. **Host scheduler / thread pumps.** Built `drivePendingTransfer`, `ioPumpProbe`, `yieldPump`, and
   worker-entry/back-edge overrides. But the guest owns its threads: 64-entry TCB table at
   `0x407160` (stride `0x210`), run loop `FUN_00151E90`, switch-in `FUN_00152200`, yield
   `FUN_0015231C`. All of it is recompiled and running. A host scheduler is by definition the thing
   this project does not have.
2. **The "transfer complete" stub.** `0x1F8480` is not a hardware status register. It is
   `return desc[2]` where `desc` comes from `FUN_001f8dd0(handle)` =
   `0x4A72C0 + (handle & 0x3F) * 0x2C`, validated against `desc[0x8]` — a **guest software queue**.
   Returning a constant 0 there fabricates guest state, and is exactly what produced the observed
   race (`count = 0` while the wait believes the transfer finished).
3. **"the runtime never retires the VIF1 DMA packet."** The 1.2 MB load is not VIF DMA. `FUN_001f8530`
   is a guest async file-read state machine: `FUN_00196b88` (open) → `FUN_00197550` (size) →
   `FUN_001970e8` (read) → `FUN_00197638` (position), looping `while (status != 3)`. The only thing it
   needs from the host is a working file server and the freedom to run.
4. **Re-entering guest functions from host callbacks.** Breaks the guest's `jal`/`jr ra` contract.
   The runtime's `eeCheckpointDue()` bail is *already* transparent by design: generated functions
   dispatch on `ctx->pc` at entry (`switch (ctx->pc) { case …: goto label_…; }`), keep all state in
   `ctx` and guest memory, and never cache guest registers in C++ locals. A bail is resumable; a
   host-driven re-entry is not.

**What the emulator probe proved (2026-11-06).** Break at `0x152434` (`jr ra` of the yield) on hardware:

```
pc = 0x152434            ra = 0x00151F88        sp = 0x00421540
t0 = 0x00407160 (TCB)    GPR context block at t0:
   +0x1A8 = 0x00151F88    <- scheduler return (set by FUN_00152200 at 0x152230)
   +0x1AC = 0x00000001    <- "alive" flag, set by the yield
   +0x1B4 = 0x001F8168    <- this thread's resume PC (worker 1, after its yield)
```

and the run-loop site it returns to:

```
0x151F78  jal FUN_0015243C    ; save
0x151F80  jal FUN_00152200    ; switch in   -> ctx[0x1A8] = 0x151F88
0x151F88  jal FUN_0015253C    ; <-- the yield lands here
0x151F90  lw  v0, 0x1AC(s0)   ; still alive?
0x151F94  bnez v0, 0x151EE8   ;   yes -> next thread
0x151F9C  jal FUN_00151CF0    ;   no  -> DELETE the thread (exit code from s0+0x200)
```

So the guest microkernel is a complete, correct round-robin: it survives threads that yield, and it
**deletes** threads that return (because a returned entry leaves `+0x1AC == 0`). Any host override that
makes a worker *return* therefore gets its thread destroyed — which is what the worker overrides did.

**Consequence for the plan.** Tear out the host scheduler machinery and the status-fabricating stub.
Then the real question is answerable: in the port, does the *guest* transfer handler
(`FUN_001f8530`) receive data from the file server, and does its descriptor ever reach status 3?
Instrument the DVCI read path, not the scheduler.

## 0b. 2026-11-06 — measured root cause of the boot-script stall

Cleanup applied first: the `0x1F8480`/`0x1F7310` "transfer complete" stub is now **opt-in**
(`TOL_VIF_STATUS_STUB=1`); the default is truthful. Added `TOL_TRACE_XFER=1`, which wraps (never
replaces) the boot-transfer read path — `open 0x196B88`, `size 0x197550`, `read 0x1970E8`,
`tell 0x197638` — and dumps the CRI handle fields.

What it measured (no stub, 60 s):

```
[xfer] size #1 a0=0x00390738 -> 266 (0x0000010A) pc=0x001F858C
[xfer] tell #1 a0=0x00390738 -> 0                 pc=0x001F8600     (x1152 and counting, always 0)
[xfer] tell #1 handle=0x00390738 state=2 dev=0x0039EE48 size=266 pos=0 dest=0x005C6240 len=544768
[load] submit #1 type=2 id=0 size=1252176 dest=0x01523010 -> handle=0x00000080
```

Reading the guest code around those numbers:

```c
// FUN_00197638 "tell"  -> simply returns handle[0x20]
// FUN_00196db0 "submit":  ... handle[0x20] = 0;  ... handle[1] = 2;   // in flight
// FUN_00196f58 "read":    if (handle[1] == 2) return 0;               // busy -> no-op
```

and the handler's own progress test at `0x1F85F8`:

```asm
0x1F85F8  jal FUN_00197638      ; tell()
0x1F85FC  addu fp, s6           ; fp = previous progress
0x1F8600  subu s6, v0, fp       ; progress = tell - previous
0x1F8610  blez s6, 0x1F8688     ; progress <= 0 -> loop again, never read
```

So the handler is **not** blocked by scheduling. It is running and spinning, and its progress
computation is `tell() - previous`. `tell` never moves off 0 because the submitted read never
completes, so `handle[0x20]` is never advanced.

This is the first hard evidence for the stall, and it is one layer lower than the scheduler:
**an asynchronous CRI/DVCI read is submitted to the device at `0x0039EE48` and never completes.**
Nothing about the guest microkernel is implicated — the yield/switch machinery was verified correct
in §0.

Corroborating negative result: generic file I/O is healthy. With `TOL_TRACE_FILEIO=1` the boot reads
all succeed (`PTN_INFO.BIN`, `MAP/SYS_REG/SND/FIELD/BATTLE/MOVIE .MBS`, correct byte counts). Note
the `[rpc] ioman fn=… -> unsupported` line is **stale and misleading**: it is printed *before*
`handleIomanFileCall` runs, and the call is handled.

Next probe: who is supposed to write `handle[0x20]` (the completion), and does the runtime's
CD/DVCI serve path deliver bytes for the request at `0x0039EE48`? Trace the device, not the scheduler.

### 0c. Answer: the completion, and whether the bytes arrive

**Who completes it.** `FUN_00197128` is the *only* function that writes `handle[0x20]` — the exact field
`FUN_00197638` ("tell") returns:

```c
// FUN_00197128
if (handle[1] == 2) {                       // read in flight
    iVar2 = FUN_0019ed88(handle[4]);        // device byte position
    handle[0x20] = iVar2 - handle[0x14];    // <- this is what "tell" reports
    FUN_00196c28(handle);
    handle[1] = 1;
}
```

It is reached from exactly three places, and every one of them gates on the state byte:

| caller | gate | calls in a 45 s run |
|---|---|---|
| `FUN_00196c98` (close) | `handle[1] == 2` | 6, none with state 2 |
| `FUN_00197408` (seek) | `handle[1] == 2` | **0** |
| `FUN_001963f8` (reap step) | reached from `FUN_00196460` only after `FUN_00197668(handle) == 3` | **0** |

**Whether the CD/DVCI path delivers bytes: yes.** Measured, not inferred:

```
[xfer] devpos #1..#16   dev=0x0039EE48 -> 1, 2, 3, 4 ...          (device position advances)
[dvci] #2  poll ... -> 0  progress=255      #3 progress=430      (one block at a time)
[dvci] #10 poll ... -> 0  progress=255      #11 511  #12 767  #13 1023  #14 1279  #15 1535  #16 1791
```

`open`/`size`/`read` all work, the state cycles 2→3 for the short transfers, and `FUN_00196c98` runs six
times successfully. So the block transport and the DVCI poll are healthy.

**CORRECTION (2026-11-06, emulator + port).** The paragraph that stood here claimed `FUN_00197128` is
the only writer of `handle[0x20]` and that nothing moves state 2→3. **Both were wrong.** A PCSX2 write
watchpoint on the state byte named the real updater:

```
watch 0x00390739 (handle+1)  -> pc=0x00197314  (FUN_001972e0), delay slot 0x197318: sb v0, 1(s0)
watch 0x0039EE49 (device+1)  -> pc=0x0019EEB4  (FUN_0019ee60): sb v0, 1(s0), v0 = 1  (busy)
```

```c
// FUN_001972e0(handle) -- the real completion, called from the DVCI tick FUN_001973a8 @0x1973DC
if (handle[1] == 2) {
    handle[1] = FUN_0019ed60(handle[4]);          // = device[1]  <-- THE GATE
    handle[0x20] = FUN_0019ed88(handle[4]) - handle[0x14];   // = "tell"
    if (handle[1] - 3 < 2) {                      // device reports 3 or 4
        handle[0x14] += (that difference);        // consumed catches up
        FUN_00196c28(handle);
    }
}

// FUN_0019ef58(device) -- where the gate value comes from
if (device[0x14] == 0) {          // remaining sectors exhausted
    device[1] = 3;                // DONE
}
```

So the chain is: `FUN_001973a8` (DVCI tick) → `FUN_001972e0` → reads `device[1]`; and
`FUN_0019ef58` sets `device[1] = 3` when `device[0x14]` hits zero. `FUN_00197128` is a different
updater, reached only from close/seek/reap — not the main path.

**Does the port reach the gate? Yes.** From `TOL_TRACE_DVCI=1` on a 70 s run:

```
[poll-in] #73 i=5 state=2 ... sDone=0    sBlocks=0 ... saw3=67  sawCount=62
[poll-in] #74 i=5 state=3 ... sDone=2048 sBlocks=1 ... saw3=68  sawCount=62
```

`saw3=68` — the state reaches 3 sixty-eight times, and the tick probe shows the handle at `b1=3`.
So `FUN_001972e0` runs, `device[1]` is read, and the gate is reached. The earlier 45 s run simply
ended before the burst and sampled `FUN_00197128` instead of `FUN_001972e0`.

**What is actually wrong now.** Each attempt completes exactly **one sector (2048 bytes)** and stops:
`sDone=2048, sBlocks=1` against `total=436462`, and `count` never moves off 0. And `tell` still
samples 0 because `FUN_001972e0` sets `pos` and immediately adds the same amount to `consumed`
(`0x14`), so by the time the handler reads it the difference is zero again. The defect is in the
block-chaining/progress accounting, not in the gate.

### 0d. Why chaining stops after one sector, and why the progress is invisible

`TOL_TRACE_XFER=1` now wraps `FUN_0019ef58` (`kAdxfReadStepPc`) and prints the six fields that decide
chaining. 60 calls, the pattern never varies:

```
[xfer] pump #1  state=2 | remain=894 chunk=1 total=894 bytes=0    done=0  win=1
[xfer] pump #2  state=3 | remain=894 chunk=1 total=894 bytes=2048 done=1  win=1
[xfer] pump #3  state=2 | remain=894 chunk=1 total=894 bytes=0    done=1  win=1
[xfer] pump #4  state=3 | remain=894 chunk=1 total=894 bytes=2048 done=2  win=1
...
[xfer] pump #60 state=3 | remain=223038 ...     bytes=2048 done=24 win=1
```

**1. Chaining stops because the window is 1.** The DONE decision in `FUN_0019ef58` is:

```c
dev[0x58] += dev[0x20];            // blocksDone += 1
dev[0x34] += dev[0x20] << 11;      // bytesDone  += 2048
if (blocksDone < remaining) {
    if ((bytesDone >> 11) < dev[0x5C]) { dev[3] = 0; return; }   // keep going
    if (0xFFFFE < dev[0x5C])           { dev[3] = 0; return; }
}
dev[1] = 3;                        // DONE
```

With `dev[0x5C] = 1` and `bytesDone >> 11 == 1`, the "keep going" test is `1 < 1` — false — so the
function falls through and reports DONE after every single chunk. The window is set by

```c
FUN_0019ee18(dev, handle[0x1C]);   // *(u32*)(dev + 0x5C) = param_2
// FUN_00196db0:  handle[0x1C] = min(requestedBlocks, handle[0xC] - handle[0x14])
```

so the request is entering `FUN_00196db0` with `requestedBlocks == 1`. (Note `dev[0x14]`, the
"remaining" the DONE test compares against, is never decremented — it stays 894 — so the only
binding constraint is the window.)

**2. The progress is invisible because `FUN_001972e0` sets and consumes it in the same call.**

```c
handle[1]    = dev[1];                              // 2 -> 3
handle[0x20] = devpos - handle[0x14];               // pos = 1 - 0 = 1
if (handle[1] - 3 < 2) {                            // TRUE, because dev[1] is 3
    handle[0x14] += devpos - handle[0x14];          // consumed becomes 1 in the SAME call
}
```

`pos` is non-zero only for the duration of that function. The handler reads `tell` later
(`FUN_00197638` → `handle[0x20]`), by which time either the fold has happened or the next submit has
reset it to 0 (`FUN_00196db0`: `handle[0x20] = 0`). Its `progress = tell - previous` is therefore
always 0, so it never copies the chunk and never advances its own remaining count.

The two are causally linked: because the window is 1 the device reports DONE on every chunk, and
DONE is exactly the condition that makes `FUN_001972e0` fold `consumed` in the same breath as `pos`.
A window > 1 would leave the device BUSY across chunks, `pos` would accumulate un-folded, and the
handler would see the progress.

**Secondary finding:** `dev[0x34]` (bytesDone) is reset to 0 between chunks (pump #2 ends at 2048,
pump #3 starts at 0). Even with a larger window, the `bytesDone >> 11` test could never accumulate —
so whoever zeroes `dev[0x34]` is a second thing to look at.

Next: find why `FUN_00196db0` is entered with `requestedBlocks == 1` (the handler computes it from
`FUN_00197550` → `FUN_0019f568` → `dev[0x14]`), and who zeroes `dev[0x34]`.

### 0e. Why the window is 1 — and where it really stops

**`dev[0x34]` is zeroed by `FUN_0019eda0`** (called from `FUN_0019ee18` inside `FUN_00196db0`, so on
every submit). It is part of the re-arm:

```c
// FUN_0019eda0(dev)
dev[0x34] = 0;                       // bytesDone, per window
dev[3]    = 0;
dev[1]    = (dev[0x14] == 0) ? 3 : 2;
dev[2] = 0; dev[0x24] = 0; dev[0x28] = 0; dev[0x47] = 1;
```

That is by design — `bytesDone` counts only the current window — so on its own it is not a defect.

**The window is the request's own block count, and that count is 1.** `FUN_00196db0` does
`handle[0x1C] = min(blocks, handle[0xC] - handle[0x14])`, and the measured call is always:

```
[xfer] submit #1 handle=0x00390738 blocks=1 ... size=894 consumed=0  size-cons=894 -> window=1 devWin=1
[xfer] submit #2 handle=0x00390738 blocks=1 ... size=894 consumed=1  size-cons=893 -> window=1 devWin=1
```

`size - consumed` is in the hundreds, so the binding operand is `blocks = 1`.

**Where the 1 comes from — and the real fault.** Every `read` in the port carries `a1 = 1`, and the
recorded callers are the **DVCI layer**, not the transfer handler:

```
read caller ra=0x00196358   3x      (DVCI step)
read caller ra=0x001967E4  10x      (DVCI poll)     all with a1 = 1
read caller ra=0x001F8720   0x      <-- the transfer handler: NEVER happens
handler-entry dumps: 1
```

On hardware the same breakpoint shows the opposite: the handler is the reader, with large, variable
chunk counts —

```
pc=0x001970E8  a0=0x00390738  a1=0x00000027 (39)   a2=0x00653940  ra=0x001F8720
pc=0x001970E8  a0=0x00390738  a1=0x000000BE (190)  a2=0x0069C200  ra=0x001F8720
```

`ra=0x001F8720` is `FUN_001f8530`'s `jal func_1970E8` at `0x1F8718` (and `0x1F85A4` → `0x1F85AC` is the
other arm). The generated code for both uses `dispatchGuestBranch`, so the probe *would* catch them.

**So the causal chain is:**

1. The transfer handler is entered once, calls `size` (`0x1F8584`, returns 266), and **never reaches
   its `read`** at `0x1F85A4` / `0x1F8718`.
2. The only reads therefore come from the DVCI layer, which asks for **1 block**.
3. `FUN_00196db0` sets `window = min(1, …) = 1`, and `FUN_0019eda0` zeroes `bytesDone`.
4. `FUN_0019ef58` sees `bytesDone>>11 == 1 >= win(1)` and reports **DONE after one 2048-byte chunk**.
5. DONE is exactly the condition that makes `FUN_001972e0` fold `pos` into `consumed` in the same
   call, so `tell` reads 0 forever and the handler's `progress = tell - prev` never fires.

So "window = 1" is a symptom. The defect is step 1: **the handler stops between `size` and `read`.**

Instrument `0x1F858C → 0x1F85A4` next: entry/exit of the handler, and whether a checkpoint bail or a
failed `dispatchGuestBranch` ends it there.

### 0f. CORRECTION — the handler's read *does* run; the device is simply never pumped

§0d and §0e measured the **DVCI background path** (1-block windows) and mistaken it for the transfer
handler's. Instrumenting the handler's own read call sites settles it.

`xferReadProbe` now logs the two handler call sites unconditionally (`ra == 0x1F85AC` / `0x1F8720`):

```
[xfer] HANDLER-READ enter ra=0x001F8720 handle=0x00390738 blocks=266 dest=0x005C6240
[xfer] HANDLER-READ exit  ra=0x001F8720 pc=0x001F8720 RETURNED-OK result=266
```

So: **neither hypothesis.** No checkpoint bail, no failed dispatch — the call went through and returned
normally. And the submit it produces is a full-window request:

```
[xfer] submit #69 handle=0x00390738 blocks=266 request=0x003B1B98 | size=266 consumed=0
       size-cons=266 -> window=266 dev=0x0039EE48 devWin=266
```

The earlier "blocks=1 / window=1" samples were the first 40 submits, all from the DVCI layer.

**The real defect is downstream: nothing pumps the armed device.** Line ordering in the same log:

```
line 277   last [xfer] pump            (FUN_0019ef58)
line 345   [xfer] submit #69           (the handler's 266-block request)
line 369+  [xfer] tell ... pos=0       (320+ polls, never advances)
```

`FUN_0019ef58` is **never called after the submit**. The device stays `dev[1] = 2`, `dev[0x58] = 0`
(position), so `FUN_001972e0` computes `pos = 0 - 0 = 0`, and the handler's `progress = tell - prev`
stays 0 forever.

The pump chain, from xrefs:

```
FUN_0019ef58  <- FUN_0019f298  <- FUN_0019f468  <- FUN_00197940  (kDvciPumpPc)
                                                   ^ called by FUN_0019ec90, FUN_0019ed08,
                                                     FUN_0019eec8, FUN_00198280, FUN_001979e0
```

and note `FUN_00197550` (size) only pumps when `handle[0xC] > 0xFFFFE`:

```c
if (0xffffe < handle[0xC]) { FUN_0019f710(dev); ... }   // 266 does not qualify
```

So the size call does not drive the device for this request either.

**Next:** find which of `FUN_0019ec90` / `FUN_0019ed08` / `FUN_0019eec8` / `FUN_00198280` /
`FUN_001979e0` is supposed to run while a transfer is outstanding, and why it stops after the
handler's submit.

### 0g. Which pump driver should run, and why it stops

**The driver chain.** `FUN_001982a0` (CRI/ADXT init) registers two callbacks:

```c
FUN_001b74c0(2, 1, 0x198260, 0);
DAT_00393238 = FUN_001b7378(4, 0x198280, 0);    // slot 4
DAT_00393230 = FUN_001b7378(5, 0x198230, 0);    // slot 5
```

and `FUN_001b7688(index)` walks that table (`0x49C6A0 + index*0x30`, 6 entries) calling each handler.
Measured dispatch:

```
slot 5  FUN_00198230  -> FUN_00198280 (slot 4 body) -> FUN_00197940 (pump)
        -> FUN_0019f468 -> FUN_0019f298 -> FUN_0019ef58
```

Two things invoke the table:

| driver | status in the port |
|---|---|
| `FUN_001996a0` → `FUN_001996b0` (the CRI thread body — a big switch over the dispatchers) | **0 calls** — never runs |
| `FUN_0019a080` (IO pump, called from `FUN_001EC670`) | runs, on the main thread |

**Why it stops.** Measured frozen counters, correlated with the handler's submit:

```
[xfer] submit #69 blocks=266 ... devWin=266 | pumps=163 cb4=70
[xfer] tell-count#320              pumps=163 cb4=70 lock=0x00000001
[xfer] cri-thread #...             0 calls
```

`pumps` and `cb4` freeze at the submit and never move again across 320 polls, and the only live
driver is the main thread's `FUN_0019a080` — which parks in the transfer wait. The CRI-thread route,
the one that would keep pumping independently, is absent.

The lock at `*(gp-0x7880)` reads 1 throughout, but `FUN_001f7390` has exactly **one** xref
(`FUN_001f8530`), so the lock is not what blocks the pump; it is simply the handler being mid-transfer.

**Conclusion:** the pump stops because its independent driver — the CRI thread `FUN_001996a0/b0` —
never runs in this port. The only remaining route shares the main thread, which is waiting.

**Next:** find how `FUN_001996a0/b0` is supposed to be entered (indirect/thread start, since it has no
literal xrefs), and why that never happens.

### 0h. `FUN_001996a0/b0` is not a thread — and the real driver is the IO pump

**What it actually is.** `FUN_001996a0` is a getter:

```c
undefined4 FUN_001996a0(void) { return DAT_003942c8; }
```

`FUN_001996b0` is not a function body either — it is the first of a block of **0x18-byte
trampolines**:

```
0x1996B0:  j FUN_001b7810
0x1996C8:  j FUN_001b7828
0x1996E0:  j FUN_001b7840   ...
```

so the block is addressed as `0x1996B0 + index*0x18`. Checked for a pointer table: no occurrence of
`0x001996B0`, `0x001996C8` or `0x001996E0` anywhere in guest RAM (three `find_pattern` sweeps over
`0x00100000..0x02000000`), and Ghidra reports no code or data xref. Nothing in this title's path
jumps there.

**The driver that does run.** The same dispatchers are reached by a direct route, confirmed by
decompiling each link:

```
FUN_001EC670                 main loop (also the frame that calls 0x19A080 on hardware)
  -> FUN_0019a080            one line: FUN_001b7870(); return;
     -> FUN_001b7870         one line: FUN_001b7688(5); return;
        -> FUN_001b7688(5)   walks 0x49C6A0 + 5*0x30, six entries
           -> slot 5  FUN_00198230
              -> FUN_00198280
                 -> FUN_00197940      (pump)
                    -> FUN_0019f468 -> FUN_0019f298 -> FUN_0019ef58
```

So the earlier phrasing "the CRI thread never runs" is wrong: there is no CRI thread in this title.
The pump's only driver is `FUN_0019a080`, invoked from the **main loop**, and it stops when the main
thread parks in the transfer wait.

`FUN_001996b0`'s trampolines are the library's alternative (jump-table) entry and are simply unused
here; probing them proves nothing about the stall.

**Next:** establish which guest thread `FUN_001660A0` (the waiter) runs on, here and on hardware. If
the waiter and the main loop are the same thread in the port but different threads on hardware, that
is the difference that starves the pump.

### 0i. Checked: the waiter IS a separate thread — that hypothesis is refuted

`FUN_001660A0` has exactly one reference, and it is a **PARAM** from `FUN_00166010`:

```c
uVar2 = FUN_00151cd0(0x1660a0, 0, 1, 0);     // create thread, entry 0x1660A0, priority 1
FUN_00152030(uVar2, &iStack_4, uVar1, 0, 0);
do {
    FUN_00151ff0(0);      // run the guest scheduler, priority 0
    FUN_00151ff0(1);      // run the guest scheduler, priority 1
    FUN_001392d0();       // frame tick (FUN_001135b8 reads GS CSR / VSync)
} while (iStack_4 == 0);
```

So the waiter is a **guest thread**, and the creator is an explicit loading loop that runs the guest
scheduler twice per iteration. That is the same structure hardware has, and the port runs it — the
scheduler was measured active and the waiter reaches its read/wait.

**So thread affinity is not the difference.** The earlier suspicion that the waiter and the main loop
are one thread is wrong.

**And the pump chain I chased is downstream of the load, not part of it.** `FUN_00166870` →
`FUN_00164EA0` → `FUN_001EC670` → 5 × `FUN_0019a080` never runs in the truthful configuration:

```
loader-wrap (0x166870)  0 calls
loader      (0x164EA0)  0 calls
cri-update  (0x1EC670)  0 calls
```

`FUN_001EC670` is the *post-load* stage (the one previously called "stage 3"), reachable only after
the wait completes, so it cannot be the pump that retires the transfer. Chasing it was a wrong turn,
and the earlier note that it "was reached" came from runs with the fabricated completion stub, which
let the loader advance past the wait.

**Where that leaves it.** The device pump during the load must come from one of the other
`FUN_0019a080` callers (`FUN_00165ad0` / `FUN_00165f60`, both of which also drive `FUN_00166010`), or
from a thread the scheduler should run. The next measurement is on the scheduler's thread set during
the wait — which guest threads exist and which are actually dispatched — rather than on more call
chains.

**Probe caveat, recorded so it is not repeated:** `replaceFunction(pc)` only intercepts calls routed
through `dispatchGuestBranch`. Generated code emits many internal calls as direct C++
(`FUN_xxx_0xNNN(...)`), which bypass the table — `FUN_001b7688` and `FUN_0019a080` are both called
that way. Every "0 calls" claim above was checked against the corpus for direct-call sites before
being used.

### 0j. MEASURED: three threads exist, all three are dispatched — and none of them is a pump

Dumping the TCB table at the first `tell` poll and logging every dispatch afterwards
(`TOL_TRACE_XFER=1 TOL_TRACE_GTHREADS=1`):

```
[gthreads] wait-begin ready0=3 ready1=0
[gthreads]   slot= 0 tcb=0x00407160 state=1 prio=0 started=1 yield=1 resume=0x001F8168   worker 1 (GS/VU1)
[gthreads]   slot= 1 tcb=0x00407370 state=1 prio=0 started=1 yield=0 resume=0x001F8528   worker 2 (handler)
[gthreads]   slot= 2 tcb=0x00407580 state=1 prio=0 started=1 yield=1 resume=0x001660EC   waiter FUN_001660A0
```

and during the wait itself, 120 consecutive dispatches split **exactly 40 / 40 / 40**:

```
[wait-turn] #6  tcb=0x00407580 resume=0x001660EC ...   waiter
[wait-turn] #7  tcb=0x00407160 resume=0x001F8168 ...   worker 1
[wait-turn] #8  tcb=0x00407370 resume=0x001F86B8 ...   worker 2
[wait-turn] #9  tcb=0x00407580 ...                     (round-robin, repeats)
```

All three `state=1` (runnable), all `prio=0`, `ready0=3`, `ready1=0`. The table is unchanged at
`wait-later` (tell #128).

**Conclusion: there is no pump thread.** The guest has exactly three live threads and the scheduler
dispatches all of them fairly; none of the three calls `FUN_0019a080`. So the DVCI pump cannot come
from the thread set at all.

**It is interrupt-driven.** `FUN_00198230` (callback slot 5) was entered with `sp=0x01FFFE10`, which
is none of the three thread stacks (worker 1 `0x00421540`, handler `0x00411480`, waiter `0x00423520`).
That is the interrupt stack, so the pump runs from an ISR — and that ISR path is what stopped at
`pumps=163`.

**Next:** measure timer-interrupt delivery during the wait (the guest ISR that reaches
`FUN_001b7688`). If the guest stops taking the interrupt, that is the defect; if it keeps taking it,
the break is inside the ISR's dispatch.

### 0k. DEFECTS FOUND

Measured during the transfer wait, all at the same instant (`tell` #1 vs #320):

```
pumps=163   cb4=70   ticks=2   lock=0x00000001   status=0x00010001
loops 4429 -> 6981     evts 4429 -> 6981     enterGuest 4258 -> 6810
```

`status=0x00010001` = IE and EIE **set**. The host run loop **is** turning, and
`g_eeProcessPendingEventsCount` tracks `g_eeRunLoopIterations` exactly, so `processPendingEvents()` runs
on every iteration. Guest code is still being entered. Yet no timer tick and no pump ever happens
again.

**D1 — the runtime latches the timer interrupt before the handler has run, and never retries.**
`PS2Memory::advanceEeTimers()` sets the flag and returns the mask:

```c
if (compareReached && (mode & CMPE) && !(mode & EQUF)) {
    timer.mode |= EQUF;             // latch set here
    interruptMask |= 1u << index;
}
```

and a latched `EQUF`/`OVFF` suppresses every later edge until the guest clears it — which only the
guest ISR does. So the *first* interrupt whose handler invocation is lost latches the timer off
permanently.

**D2 — a queued interrupt is marked delivered before it is executed.** `EeScheduler::dispatchIrq()`:

```c
if (!matching.empty())
    pending &= ~causeBit;           // cleared because a handler was FOUND
for (const EeIrqHandler &handler : matching)
    queueInvocation(std::move(invocation));
```

and `processPendingEvents()` does `m_pendingEeTimerInterrupts = 0u;` before dispatching. So the
pending state is destroyed at *queue* time, not at *completion* time. If the invocation is never
executed, nothing re-raises it — and because of D1, nothing can.

Together D1+D2 make a single dropped invocation permanent: timer latched, pump dead, waiter parked
forever. That matches every measurement: `pumps`/`ticks` freeze while the host loop keeps turning.

**D3 — `FUN_001996a0/b0` was misidentified as a missing thread.** It is a getter plus a block of
0x18-byte trampolines (`j FUN_001b7810`, …) addressed as `0x1996B0 + index*0x18`; no pointer to any of
them exists in RAM and nothing jumps there. The title has exactly three guest threads (worker 1,
worker 2, waiter) and the scheduler dispatches all three 40/40/40. There is no pump thread to miss.

**D4 — the "transfer complete" stub.** Fixed this session: `0x1F8480` reads the guest's own
descriptor queue (`0x4A72C0 + (handle & 0x3F) * 0x2C`), not a hardware register, so answering it with a
constant fabricated guest state. Now opt-in behind `TOL_VIF_STATUS_STUB`.

**D5 — probe blind spot worth knowing.** `replaceFunction(pc)` only sees calls routed through
`dispatchGuestBranch`; many internal calls are emitted as direct C++
(`FUN_xxx_0xNNN(...)`). `FUN_001b7688` and `FUN_0019a080` are both called that way, so a zero count
must always be cross-checked against the corpus.

**Next:** confirm D2 by checking whether the executor drops invocations queued while a guest thread is
suspended inside the guest microkernel, then fix the acknowledgement ordering (clear on completion, not
on queue) and the timer latch (only latch when the handler actually runs).

### 0l. CONFIRMATION RESULT — D2 refuted, D1's premise not present

Added `TOL_TRACE_INVQ=1` in `EeScheduler::run()` (prints the pending-invocation queue depth when it
changes) and dumped the EE timer registers from the waiter.

**The invocation queue drains.** It never grows:

```
[invq] cur=0 inGuestCall=0 q=0
[invq] cur=1 inGuestCall=0 q=1
[invq] cur=1 inGuestCall=0 q=0
[invq] cur=1 inGuestCall=0 q=2
[invq] cur=1 inGuestCall=0 q=1
[invq] cur=1 inGuestCall=0 q=0        max q ever seen = 2
```

`inGuestCall` is 0 throughout. Invocations are queued by `dispatchIrq` and consumed. **So D2 is
wrong**: nothing is dropped or starved.

**The timer is not latched.** Timer 2 during the wait:

```
t2 = 53971/898/0  ->  54291/898/0        (count / mode / target)
```

`mode = 0x382` = CUE(0x80) | CMPE(0x100) | OVFE(0x200); **EQUF(0x400) and OVFF(0x800) are clear**.
So the timer is counting normally and no flag is stuck. `ticks=2` earlier was not evidence of a stuck
interrupt — with `target = 0` the compare period is a full 65536-tick wrap, so the tick loop simply was
not due. **D1 as stated does not hold either.**

**What is confirmed.** Both late-bound things the guest depends on are healthy throughout the wait:

```
frames (FUN_001392d0)   36 -> 291      loading loop turns every frame
host loops              4429 -> 6981   evts == loops
t2 count                53971 -> 54291 timer alive
status                  0x00010001     IE + EIE set
pumps                   163 -> 163    FROZEN
```

So the port's scheduler, frame loop, interrupt delivery and timers are all working. The DVCI pump is
simply never called again after the handler's submit — the guest-side **trigger** for it stops being
true.

**Corrected next step:** stop looking at the runtime. Find the guest condition that gates the
`FUN_0019a080` / `FUN_001664d0` call on the main thread and identify what changes at the submit (the
descriptor state it tests, or a flag the handler is supposed to clear). D1/D2 as written should be
treated as refuted hypotheses, not defects.

### 0m. The load stage has no pump — by design

Decompiled the real main loop, `FUN_00165ad0`:

```c
FUN_00165c50();
FUN_00165c90(..);                       // boot: init, 5 setup frames, FUN_001663a0 (the boot pump path)
do {
    uVar1 = FUN_00166010();             // LOAD STAGE
    FUN_00166120(uVar1);
    FUN_001667e0();
    FUN_00166870();
    ...
    do {                                // FRAME LOOP
        FUN_00152b70();
        FUN_00151ff0(0);  FUN_00151ff0(1);  FUN_00151ff0(4);
        ...
        FUN_0019a080();                 // <-- the IO pump lives ONLY here
        ...
        lVar2 = FUN_00153180();
    } while (lVar2 == 0);
} while (true);
```

and the load stage, `FUN_00166010`:

```c
uVar2 = FUN_00151cd0(0x1660a0, 0, 1, 0);        // waiter thread
FUN_00152030(uVar2, &iStack_4, uVar1, 0, 0);
do {
    FUN_00151ff0(0);                            // priorities 0 and 1 only
    FUN_00151ff0(1);
    FUN_001392d0();                             // frame tick
} while (iStack_4 == 0);                        // no FUN_0019a080 anywhere
```

`FUN_001392d0` → `FUN_001135b8` → `FUN_00114b70`, which is a bare spin with no pump and no yield:

```c
do { uVar1 = REG_INTC_STAT; } while ((uVar1 & 4) == 0);
```

The waiter itself only yields (`FUN_001660a0`: `while (status != 0) FUN_0015231c();`).

**So the DVCI pump is unreachable while a load stage is running.** The freeze at `pumps=163` is the
guest behaving as written, not a scheduling failure. The load must be retired by the three guest
threads alone.

**The one pump that does happen during a load is the submit itself.** `FUN_00196db0`:

```c
lVar2 = FUN_0019ed60(dev);
if (lVar2 != 1) { FUN_0019eec8(dev); }     // FUN_0019eec8 -> FUN_00197940 (pump)
```

which is exactly when the counter jumps: `pumps=163` at `submit #69`.

So the design is *one pump per submit*. To make progress the handler must submit again — and it only
submits again once `tell` advances — and `tell` only advances when the device is pumped. That is the
closed loop.

**Next:** determine how far one `FUN_0019eec8` → `FUN_0019f298` → `FUN_0019ef58` call advances the
device (one block? one window?), and therefore how many pumps the 266-block window needs. If it is one
block per call, the submit's single pump can never finish a 266-block window and the design needs the
handler to re-submit after each block — which is what `tell`/`consumed` is supposed to signal.

### 0n. Narrowed: the device is left "armed but not started"

`FUN_0019eec8` is a blocking drain loop, and its entry test decides everything:

```c
void FUN_0019eec8(int dev) {
    FUN_0019ee60(dev);                              // status check
    do {
        do { FUN_00197940(); }                      // pump
        while (*(char *)(dev + 1) != 1);            // until device idle
    } while (*(int *)(dev + 0x24) != 0);
}

// FUN_0019ee60
if ((*dev & 0xFFFF00) == 0x10200) { dev[0x48] = 1; dev[0x47] = 0; }   // in progress
else                              { dev[1] = 1; }                    // -> drain exits at once
```

`*dev` is a packed byte field, so `(*dev & 0xFFFF00) == 0x10200` means **dev[1] == 0x02 and
dev[2] == 0x01**. dev[2] is `FUN_0019ef58`'s own re-entrancy flag — the pump sets it when it starts a
transfer.

Measured difference at the handler's submit:

```
port      dev0 = 0x00000201   -> dev[1]=2  dev[2]=0   "armed, NOT started"
hardware  dev0 = 0x00010201   -> dev[1]=2  dev[2]=1   "in progress"
```

**The next blocker is a catch-22:**

1. `FUN_00196db0` arms the read: `FUN_0019ee18` → `FUN_0019eda0` sets `dev[1]=2`, `dev[2]=0`.
2. Nothing starts it, so `dev[2]` stays 0.
3. The only pump entry available during a load stage is `FUN_0019eec8`'s drain loop, and it checks
   `dev[2]` first — 0 means "no work", so it exits immediately.
4. `FUN_0019ef58` (which would set `dev[2]=1`) is only reachable through `FUN_00197940`, i.e. through
   that same drain loop.

So the device is left in the one state no drain path recognises as work, and the only function that
could move it is gated behind that recognition.

**Two candidate fixes to test, in order:**
- `FUN_00196db0` arms and then starts: one `FUN_00197940` after `FUN_0019ee18` (or `FUN_0019eda0`
  leaving `dev[2] = 1`).
- `FUN_0019ee60` treats `dev[1] == 2` as "work remains" even when `dev[2] == 0`, so the drain loop runs
  the pump once and starts the transfer.

### 0o. Attempted fix #1 — FAILED, and it is instructive

Implemented "start after arm" as an override on `FUN_00196DB0` (`installXferStartFix`): call the original,
and if the device is left `state==2 && busy==0`, call `FUN_00197940` once with a sentinel `$ra` so a bail
propagates exactly like generated code does.

Result — deterministic, 3 runs each:

```
without wrapper (TOL_XFER_NO_START=1)   submits=41  handlerRead=1
with wrapper                            submits=2   handlerRead=0
```

and the wrapper's own diagnostic showed it entered **once**, with `state=1, busy=0`, and returned early
(the trigger condition never matched). So it never performed its action, yet merely existing cut the run
from 41 submits to 2.

**The fix is therefore harmful and is now opt-in** (`TOL_XFER_START=1`); the default path is back to the
known-good 41 submits with the handler running. Verified after the change.

**What this tells us:** replacing the table slot for `FUN_00196DB0` perturbs the submit path by its
presence alone. The likely reason is that the function is entered through more than one route — the slot
at `0x196DB0` plus interior addresses / direct calls — so half the paths get the wrapper and half the
original, and the two disagree about `ctx->pc`. That must be understood before any fix is placed on this
function; the next attempt should not wrap it at all.

**Untried and still the better candidate:** leave `FUN_00196DB0` untouched and fix the *drain* instead —
override `FUN_0019EEC8`, which is only reached from 7 call sites and whose purpose is exactly "drain the
device", so a bounded "if the device is armed, run the pump" is safe to add there without touching the
submit.

### 0p. Drain override: safe and informative; frame-tick start: ineffective

**The drain override is safe.** Wrapping `FUN_0019EEC8` (pure observation) leaves the run unchanged:

```
submits=41  handlerRead=1     (identical to baseline, 2 runs)
[drain] 72 calls
```

and it answers the open question:

```
[drain] #1 in{st=3 busy=0 rem=894 chunk=1 req=0 done=1 win=1} out{st=1} BAILED
[drain] #3 in{st=1 ...} out{st=1}
entries with st=2 (armed): 0
entries with win=266:      0
maxDone=31  maxWin=1
```

**The drain never sees the handler's request.** Every call is the DVCI background path (window 1). So
draining cannot fix it — and calling the drain on an armed device would be worse: `FUN_0019ee60` sees
`st=2, busy=0`, writes `st=1` and the loop exits, discarding the armed read.

**Frame-tick start is ineffective.** `installFrameStartFix` wraps `FUN_001392D0` and, when the device is
`st==2 && busy==0`, calls the pump once. It installs and chains correctly (`frame-start installed` +
`probe installed`), the baseline is preserved (`submits=41 handlerRead=1`), but the hook never fires and
its per-frame diagnostic (`frame-see`) never prints. So `FUN_001392D0` is not running during the stall —
which contradicts the earlier `frames 36 -> 291` reading and needs reconciling before another hook point
is chosen.

**Also confirmed:** the submit wrapper (`FUN_00196DB0`) is the harmful one — with it removed the run is
back to 41 submits with the handler entering.

**State of the tree:** every behavioural wrapper is opt-in (`TOL_XFER_START`), none are installed by
default, and the default path is verified at 41 submits / handler read.

**Next:** measure `frames` (the `FUN_001392D0` counter) and `submits` in the *same* run, so the load-stage
loop and the handler submit are placed on one timeline. Only then pick the tick that actually runs during
the stall.

### 0q. Timeline: the poll is boot-only and the handler submits one frame too late

Every probe now stamps `gFrameTicks`, so submits and polls share one axis:

```
poll#1 .. #74      all frame=35     (74 DVCI polls, then never again)
submit#1 .. #40    all frame=35     blocks=1,   iopump 0->39, pumps 2->92
submit#69          frame=36         blocks=266, iopump=68, pumps=163   <- the handler
tell#256           frame=291        iopump=68, pumps=163              (frozen)
```

Three facts fall out:

1. **The DVCI poll driver is boot-only.** All 74 polls land inside frame 35 and then the poll is never
   called again. `FUN_001664D0` is entered once (measured earlier) and loops internally; it is reached
   only from `FUN_001663A0` ← `FUN_00165C90`, the boot sequence. So the poll cannot serve the handler's
   later request.

2. **The handler submits one frame after the background burst ends**, at frame 36, when the device has
   already been drained back to idle (`dev[1] == 1`, measured at the submit).

3. **That matters because the submit only drains when the device is already busy.** `FUN_00196db0`:

```c
lVar2 = FUN_0019ed60(dev);        // dev[1]
if (lVar2 != 1) { FUN_0019eec8(dev); }   // drain whatever is in flight
... arm the new read ...
```

   With `dev[1] == 1` the drain is skipped, then the read is armed and nothing ever pumps it. Had the
   submit landed while the previous operation was still in flight, that same call would have drained the
   device instead.

**So the shape of the defect is timing, not a missing call:** the port finishes the background stream
and goes idle before the handler submits, whereas the design expects the handler's submit to overlap a
busy device. That is what makes the single drain-on-submit insufficient.

**Next:** confirm on hardware that the handler's submit lands with `dev[1] != 1` (i.e. overlapped), and
find what keeps the device busy there. If that holds, the fix is to make the armed read start without
depending on the previous operation still being in flight.

---

## 1. Build & run

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && ninja -C build/tol2 -j 12 tol'
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

- Disc image `data/CD/CDVD.000`; ELF `data/CD/SLUS_212.01`.
- `data/output_fixed/` is **git-ignored / generated**. Local edits there are artifacts; durable
  changes belong in the recompiler (`thirdparty/PS2Recomp/ps2xRecomp`).
- Full corpus rebuild ≈ 5–10 min (303 objects). Single-file corpus edits rebuild in seconds.

---

## 2. Diagnostics (opt-in, zero cost when unset)

| Env var | What it shows |
|---|---|
| `TOL_TRACE_SPIN=1` | samples `pc/ra` every 200k backward branches — **first tool to reach for** |
| `TOL_TRACE_PC=0xADDR,...` | enter/exit + args for those guest addresses |
| `TOL_TRACE_LOAD=1` | load jobs, resource lookups, transfer submits, preempt/resume, stale-lock clears |
| `TOL_TRACE_SCRIPT=1` | script handle base/index/count, peeked opcode, null-block guard |
| `TOL_TRACE_DVCI=1` | DVCI handle/stream dumps, ADXF arms, io-wait, pump health, 40-slot stream dump |
| `TOL_TRACE_GS=1` | GS worker steps, completion flag, worker-2 steps |
| `TOL_VIF_STATUS_REAL=1` | use real VIF status instead of the forced "transfer complete" |

Triage: run with `TOL_TRACE_SPIN=1` and look at the last 10 `[spin]` lines. Same `pc` = stuck;
moving = working (but maybe slow).

### Ground truth via PCSX2

The `pcsx2_*` MCP tools work here. `pcsx2_connect` (mode `auto`) attaches DebugServer (21512) + Pine
(28011). **Always `pcsx2_save_state` before continuing, and `pcsx2_load_state` afterwards** — it is a
live user session.

Verified values on real hardware:

| Address | Real hardware |
|---|---|
| `0x003DB6BC` (resource table ptr) | `0x01523CA0` (non-null) |
| `0x01523010` (boot script buffer) | `0xF6F6F6F6`, `0x00084916`, `0x00131B40`, … |
| `0x00394378` / `0x00394380` (park flags) | `0` / `0` |

Registers captured at a breakpoint on `0x0015231C`:

```
PC = 0x0015231C    ra = 0x001F8168    <- inside worker 1's own loop
sp = 0x00421540    t0 = 0x00407160    (thread table)
t2 = 0x000001D0    (context size)     t3 = 0x000001C0
gp = 0x003E2BF0
```

---

## 3. What is already fixed (verified)

| # | Fix | Where |
|---|---|---|
| 1 | **Jump dispatch fell through** after `dispatchGuestBranch(..., IndirectJump/DirectJump)`, restarting switch case 0 forever (`FUN_0028B010`'s `jr v0` table). Emitter now emits `return;`. 1324 sites / 302 files patched in the generated corpus. | `thirdparty/PS2Recomp/ps2xRecomp/src/lib/control_flow_emitter.cpp`; `data/output_fixed/*` |
| 2 | **Script null command block** made the dispatcher walk ~1.07e9 indices (`count` read from guest addr 0 = `0x3F800000`). Now reports "finished". Installed on the **thunk `0x1F7340`** — it calls `FUN_001f88f0` by direct C++ call, so overriding `0x1F88F0` never fires. | `scriptStatusProbe` on `0x1F7340` |
| 3 | **Worker 2 (`0x1F84B0`)** infinite thread loop bounded at its **back edges** (`0x1F8518`, `0x1F8528`) by running the real epilogue, so the loop body executes as guest code. Replaced an earlier host-side coroutine that silently dropped transfers. | `installWorker2BackEdge` |
| 4 | **Transfer queue never drained** — now pumped from the I/O wait. | `ioPumpProbe` on `0x0019A080` |
| 5 | **Stale transfer spinlock** (`FUN_001f7390`, `*(gp-0x7880)`): a checkpoint inside the handler left it held by the same thread. Cleared before acquiring. | `transferLockProbe` on `0x001F7390` |
| 6 | **Yield pumped nothing**: `FUN_0015231C` now also runs the DVCI/ADXF pump. *Possibly redundant now that the guest scheduler is understood — see §5.* | `yieldPump` on `0x0015231C` |

---

## 4. The load chain, with evidence

```
FUN_00165AF0 (startup frame)
 ├─ FUN_00165C90   CRI init + AFS extent tables + logos     OK
 ├─ FUN_00165E70   frame update                             OK (runs once)
 ├─ FUN_00166010   builds the script buffer                 blocked
 ├─ FUN_00166120   script dispatcher (opcode 0 sets up the resource table)
 ├─ FUN_001667E0
 ├─ FUN_00166870   -> FUN_00164EA0 -> FUN_001EC670          blocked
 └─ FUN_00139D50 ...                       (0x165B54, never reached)
```

**4.1 The boot script is a 1.2 MB transfer that never lands.**

```c
size = FUN_001f72d0(0);            // resource 0 size + 0x10
buf  = FUN_001013e0(0, size);      // the script block at 0x01523010
thr  = FUN_00151cd0(0x1660a0, 0, 1, 0);
FUN_00152030(thr, &flag, buf, 0, 0);
while (flag == 0) { ... }
return buf;                        // becomes FUN_00166120's a0
```

```
[load] submit #1 type=2 id=0 size=1252176 dest=0x01523010 -> handle=0x00000080
```

`*(0x01523010)` stays `0` ⇒ the dispatcher sees `count = 0` and executes no opcodes.

**4.2 The resource table therefore stays null.**

- `FUN_003710C0(id) = *(u32*)(table + 0x10 + id*4) >> 2`, `table = *(gp - 0x7534)` = `0x003DB6BC`
  (zero-filled `.bss` tail of the single PT_LOAD — must be set at runtime).
- Only writer: `FUN_003710A0`, only caller `FUN_0027A990`, only reached as **script opcode 0**.
- Observed: `id=1..3 -> 0` (skipped); `id=4 -> 0x0FE00000` (= `0x3F800000 >> 2`, read from addr
  `0x20`), creating a bogus job. `FUN_001EC670` stage 3 then waits forever (`state 1 -> 0`, never `2`).

**4.3 The transfer handler.** `FUN_001f8530`: spinlock → `desc[2]=1` → `FUN_00196b88(id)` →
`FUN_00197550` → `FUN_001970e8` → wait for DVCI state 3 → copy/decompress → write `0xF6F6F6F6` at
`dest` → `desc[2]=0`.

---

## 5. The threading model (the real architectural finding)

**Do not write a second scheduler. The guest already has one, and it is the thing to extend.**

| Symbol | Role |
|---|---|
| `FUN_00151FF0(prio)` | scheduler entry: `if (FUN_001520a0(prio)) FUN_00151e90(prio);` |
| `FUN_001520A0(prio)` | ready count = `*(u16*)(0x48F570 + prio*2)` |
| `FUN_00151E90(prio)` | run loop: pops run queue, sets current, calls `tcb[0x1B0]` **once**, then `FUN_0015243C` / `FUN_00152200` / `FUN_0015253C` |
| `FUN_00151780` / `FUN_001517C0` | run-queue append (links `+0x1F8`/`+0x1FC`) / pop |
| `FUN_001517E0` | subsystem init: zeroes `0x48F590` (0x7400), `0x407160` (0x8400), clears `0x48F570` |
| `FUN_001518C0` | thread create: guest stack allocator (bitmap `0x48F560`/`0x40F560`), TCB fill, enqueue |
| `FUN_0015243C` | spills the full register file into `0x48F590 + a*b` |

- TCB: base `0x407160`, **stride `0x210`**; state `+0x1F0`, priority `+0x1F1`, entry `+0x1B0`,
  entry arg `+0x1C0`, started flag `+0x1F3` (offset 499), `+0x1AC`, `+0x200`.
- Context: base `0x48F590`, size `0x1D0`; `+0x1AC` state, `+0x1B4` `$ra`, `+0x1A0` `$sa`.
- `FUN_0015231C` is **save-only** — verified on hardware (`ra=0x001F8168`, i.e. it returns into the
  caller's own loop). It is *not* the switch.
- Threads have **their own guest stacks** (`sp=0x00421540` vs main `0x01FFxxxx`).

### Where the remaining gap is

The wait loops **already** pump the scheduler (`FUN_001EC670` / `FUN_00166010` call
`FUN_00151FF0(0)` and `(1)`), so the gap is not "call the scheduler" but the **stack switch inside the
run loop** — `FUN_00152200` / `FUN_0015253C` restoring a context (including `sp`) and resuming it.

Next checks, in order:

1. Break at `0x152200` and `0x15253C` in **PCSX2** and in this port (`TOL_TRACE_PC`) — are they reached?
   If they are reached here, the bug is in how the restored `sp`/`pc` is honoured by the runtime.
2. Check priority coverage: `FUN_001EC670`'s wait loop calls `FUN_00151FF0(0)` only, while **worker 1
   (`0x1F8150`) is priority 1**. If a wait loop needs a priority-1 thread to run, it never will.
3. Consider whether the port's entry overrides (`0x1F8150`, worker-2 back edges) are now *masking* the
   guest scheduler. They were added when nothing scheduled those threads; once the scheduler runs, they
   may be unnecessary — test by disabling them (`TOL_TRACE_GS` shows worker step counts).

### Path A applied — host pumps off, guest scheduler verified running

Host pumps (`yieldPump`, `ioPumpProbe`) are now **opt-in** behind `TOL_LEGACY_WORKER_PUMPS`. Default is
Path A: yields go to the guest scheduler.

Measured in this port with Path A:

```
0x151FF0 scheduler entry = 32     0x152200 context restore = 40     0x1517C0 queue pop  = 40
0x151E90 run loop        = 31     0x15243C context save    = 40     0x151780 queue append = 4
```

So the guest microkernel **does** run here — 40 real save/restore cycles.

`FUN_00152200` is confirmed as the switch (from its generated code):

```c
ctx = *(u32*)(0x48F590 + idx);
ctx[0x1A8] = retaddr;  ctx[0x1AC] = 0;
pmthi(ctx[0x180]); pmtlo(ctx[0x190]); mtsa(ctx[0x1A0]);
/* restore f0..f31 and GPRs from ctx, then */
jr ctx[0x1B4]          // IndirectJump -> the thread's saved $ra
```

The save half (`FUN_0015231C`) writes `ctx[0x1B4] = $ra`, so together they are a genuine cooperative
context switch, and the recompiler models it correctly (`IndirectJump` at `0x152314`).

**Address correction:** the transfer handler's DVCI calls are `0x196B88` (open) and `0x1970E8` (read) —
*not* `0x1F96B88`. Earlier traces using the `0x1F96B88` spelling always read 0 because no such
function exists. Don't repeat that.

With Path A the handler now genuinely reaches DVCI (handler ×2, DVCI open ×2, decompressor `0x1F7480`
×1, reap `0x1F8DA0` ×1) — before, it bailed at `0x1AD2D8` without reaching any of it. **But the boot
script buffer still reads `count = 0` after 90 s**, so the transfer still does not complete.

**Next:** determine whether descriptor #1 (boot script) was reaped or only #2, and where the second
handler pass stops. Watch the transfer status byte `desc[2]` at `0x4A72C0` and trace
`TOL_TRACE_PC=0x1F8530,0x196B88,0x1970E8,0x1F97668,0x1F8DA0`.

### Answer: the boot script IS reaped — the failure is a race

Descriptor trace (`TOL_TRACE_LOAD=1`, descriptor tracker on `0x1F8DA0` + `0x1F8530`):

```
submit #1  id=0           dest=0x01523010  handle=0x80   -> slot 0 (boot script)
submit #2  id=266338304   dest=0           handle=0x101  -> slot 1 (garbage)

handler-entry[0] desc=0x4A72C0 status=2    <- slot 0 queued
reap-in         desc=0x4A72C0 status=0    <- slot 0 COMPLETED and reaped
before[1]       desc=0x4A72EC status=2    <- slot 1 still queued
```

So the 1.2 MB boot-script transfer **succeeds**. The bug is a **race**: `FUN_001660A0` sets its
completion flag as soon as `FUN_001f8480` returns 0, and that function is still the forced-done stub,
so `FUN_00166120` reads `0x01523010` before the transfer lands → `count = 0`.

Descriptor layout recap: `0x4A72C0 + slot*0x2C`; `+0` queued flag, `+1` kind, `+2` status
(2 queued / 1 busy / 0 done), `+4` id, `+8` handle, `+0x10` size, `+0x1C` dest.
Handle = `slot | (idx << 7)`, so slot = `handle & 0x3F`.

### Negative result: pumping the scheduler from the status poll does NOT work

Tried `drivePendingTransfer` (installed on `0x1F7310` thunk + `0x1F8480`): report the real status, and
while the descriptor is pending run `FUN_00151FF0(0)` so the transfer advances.

```
[load] transfer status wait installed (pumps guest scheduler)
[xfer] status-wait #1 slot=0 status=2 -> pumping guest scheduler
[xfer] handler-entry[0] desc=0x4A72C0 status=2 ...
        (no reap, no [script] status at all)
```

It fires, but the handler never completes and the script dispatcher is never reached — strictly worse
than the stub (which at least continues with `count = 0`). **Reverted**: the always-done stub is back
as the default; the waiter is retained behind `TOL_VIF_STATUS_WAIT=1` (which also needs
`TOL_VIF_STATUS_REAL=1`) purely for further experiments.

Open question that this rules out: the transfer does not merely need "more scheduler time during the
status poll". Something about consuming the transfer from inside that call is wrong — most likely the
handler must run on its own thread context/stack via the normal run loop, not nested inside
`FUN_001f8480`.

Suggested next probe: put a breakpoint on `0x1F8530` in **PCSX2** and check whether, on hardware, the
handler runs at all inside `FUN_001660A0`'s wait — or only later from the run loop. That decides
whether the fix is to wait differently or to let the flag be set later.

### PCSX2 answer: the handler runs only from the worker's own thread

Breakpoint on `0x1F8530`, `pcsx2_get_backtrace`:

```
Call stack (3 frames):
#0  0x001F8530  sp=0x00411530   (transfer handler)
#1  0x001F84B0  pc=0x001F84F8   sp=0x004115E0   (worker 2's `jal 0x1F8530`)
#2  0xFFFFFFFF  pc=0x00000000   sp=0x00411600   <-- no caller: thread stack bottom
```

So on hardware the handler is entered **only** from worker 2's loop, on the worker's own stack, with
**no caller frame** — it is a thread entry, never a nested call. It is *never* invoked inside
`FUN_001660A0`'s wait.

That confirms the failed `drivePendingTransfer` experiment was architecturally wrong: the transfer
must not be consumed from inside the status call.

Supporting static facts:

- `FUN_00152200` (the context switch) has exactly **one** caller: `FUN_00151E90` (the run loop,
  at `0x151F80`).
- `FUN_00151FF0` (scheduler entry) has 18 callers, **none of them the loader thread** (`0x1660A0`).
- `FUN_001660A0`'s wait only calls `FUN_0015231C`, which is save-only and returns into its own loop.

Therefore a spinning thread can only get back to the run loop if it **returns** — otherwise it must be
**preempted**. The runtime does have EE timer emulation (`PS2Memory::advanceEeTimers`,
`cyclesUntilNextEeTimerInterrupt`, `EeScheduler::m_pendingEeTimerInterrupts`, `EeTimer[4]`), so timer
preemption is the plausible mechanism, but the guest ISR path that would call `FUN_00151FF0` has not
been verified.

**Next concrete step:** confirm whether a pending EE timer interrupt actually reaches guest code and
ends up invoking `FUN_00151FF0` (break on `0x151FF0` and on the timer ISR in PCSX2, and check
`m_pendingEeTimerInterrupts` handling in `EeScheduler`). If preemption is the missing piece, the fix
is interrupt delivery, not another transfer waiting hack.

### PCSX2: the load wait DOES pump the scheduler — and our back-edge breaks the livecycle

Backtrace at `0x151FF0`:

```
#0  0x00151FF0  sp=0x01FFECE0   (scheduler entry, main stack)
#1  0x001EC670  pc=0x001EC6D4   sp=0x01FFED00   <- the load-stage wait loop
#2  0x00165AD0  pc=0x00165B54   sp=0x01FFED30   <- startup frame at 0x165B54
#3  0x0008FEFC  pc=0x00100210   sp=0x01FFED60
```

So the **main thread pumps `FUN_00151FF0` from `FUN_001EC670`'s wait loop** (xref at `0x1EC6CC`).
Our port does the same (32 calls). Pumping the scheduler is therefore correct, and the earlier
`drivePendingTransfer` idea failed for a different reason.

`FUN_00151E90`'s thread lifecycle (from its decompile):

```c
if (pkt[0x1F0] == 1 /* runnable */) {
    currentThread = pkt;
    if (pkt[0x1F3 /* offset 499 */] == 0) {      // only ever ONCE per thread
        pkt[0x1F3] = 1;
        pkt[0x1B0](pkt + 0x1C0);                 // the thread entry
    }
    FUN_0015243C();   // save current context
    FUN_00152200();   // restore next context (the switch)
    FUN_0015253C();
}
```

A thread entry is invoked **exactly once**, and every later resume happens through
`FUN_00152200` restoring the thread's saved context. A thread that **returns** from its entry is not
part of that contract.

**This is the likely bug in the port:** `installWorker2BackEdge` makes worker 2 return from its entry
after one iteration, so the microkernel believes the thread finished and never resumes it — matching
the measurement (handler ×2, reap ×1, then nothing).

Next fix to try: make the back edge a **yield** rather than a return — i.e. save the context
(`FUN_0015231C`) and hand control back to the run loop so the thread stays runnable and is resumed
via `FUN_00152200`. Confirm with a PCSX2 breakpoint on `0x15253C` to see exactly how a yield returns
to the scheduler.

### The switch contract, fully confirmed (read it before changing anything)

Disassembly of the yield tail and the matching generated C++:

```asm
; FUN_0015231C (yield / switch OUT)                       generated C++ (identical)
0x15242C  sw   ra, 0x1B4(t0)     ; save this thread's resume PC   WRITE32(t0+436, ra)
0x152430  lw   ra, 0x1A8(t0)     ; load the scheduler return      SET_GPR_S32(ctx,31,READ32(t0+424))
0x152434  jr   ra                ; -> back into FUN_00151E90       ctx->pc = jumpTarget; return;
```

```asm
; FUN_00152200 (switch IN)
0x152230  sw   ra, 0x1A8(t0)     ; store the scheduler return address
...
         jr   ctx[0x1B4]         ; jump to the thread's saved resume PC
```

So the pair is symmetric and **both halves are already generated correctly by this port**. The
microkernel is not broken at this level. Note the corollary: a thread's **first** entry must go
through `FUN_00152200` so that `ctx[0x1A8]` is populated; a thread entered by a raw call and then
yielding would jump to a stale/zero `ctx[0x1A8]`.

Verified measurements in the port with the microkernel active (host pumps off):

```
0x151FF0 scheduler entry = 32    0x152200 restore = 40    0x1517C0 queue pop = 40
0x151E90 run loop        = 31    0x15243C save    = 40
```

Status summary at this point:

- Forced-done stub (default): flow proceeds, but `count = 0` — the read races the transfer.
- Real status (`TOL_VIF_STATUS_REAL=1`): the wait never completes, even with a 180 s window.
- The transfer itself succeeds and is reaped (descriptor trace, §5).
- The switch mechanism is correct in the port.

**No code change was made from these findings** — there is no obvious defect to patch. The next
useful probe is a PCSX2 breakpoint at `0x152434` (`jr ra` in the yield) to read the actual jump
target, and a matching `TOL_TRACE_PC` run to see whether the port's yield lands on the same address.
If they differ, that is the bug; if they match, the problem is in run-queue re-selection
(`FUN_001517C0` / `FUN_0015253C`) and should be looked at there.

### Latent hazards introduced along the way

- `yieldPump` runs the DVCI pump on every `FUN_0015231C`. With a working scheduler this is probably
  wrong (double-pumping) — re-evaluate once the scheduler runs.
- `ioPumpProbe` calls worker 2 from `FUN_0019A080`. Also re-evaluate for the same reason.
- `TOL_VIF_STATUS_REAL=1` switches `FUN_001f8480` from forced-done to real status. Real status
  deadlocks in `FUN_001660A0`'s yield-only wait, which is another symptom of the same missing switch.

---

## 6. Success criteria (in order)

1. `*(0x01523010) == 0xF6F6F6F6` and the next word looks like a command count.
2. `[script] status` reports non-zero `count`, then opcode traffic.
3. `FUN_0027A990` runs ⇒ `0x003DB6BC` becomes non-zero and `[load] resource id=1..4` return non-zero.
4. `FUN_001EC670` stages complete: `[load] status` reaches `state=2`.
5. `FUN_00139D50` entered from `0x165B54`.
6. First logo appears (5 sprites drawn by `FUN_00165C90`, not movies).

---

## 7. Key addresses

| Address | Role |
|---|---|
| `0x00165AF0` | startup frame (`0x165B2C`→`FUN_00166120`, `0x165B4C`→`FUN_00166870`, `0x165B54`→`FUN_00139D50`) |
| `0x00165C90` | CRI init + AFS extent tables + logos |
| `0x00166010` | builds the script buffer, returns it as the dispatcher's `a0` |
| `0x001660A0` | loader thread entry: starts the transfer, waits on `FUN_001f8480` |
| `0x00166120` | script dispatcher (`opcode 0` → `FUN_0027A990`) |
| `0x00166870` | → `FUN_00164EA0` → `FUN_001EC670` |
| `0x001EC670` | five sequential load stages; stage 3 blocks |
| `0x0027A990` | script opcode 0 handler → sets the resource table |
| `0x003710A0` | `*(gp-0x7534) = arg`, the only resource-table writer |
| `0x003710C0` | resource lookup `(table + 0x10 + id*4) >> 2` |
| `0x001F72D0` | resource size lookup |
| `0x001F72F0` → `0x001F82F0` | build transfer descriptor (id `+4`, size `+0x10`, dest `+0x1C`) |
| `0x001F8BE0` | queue descriptor at `0x4A72C0 + slot*0x2C` (status `+2`, handle `+8`) |
| `0x001F8AC0` | priority-queue insert, head `*(gp-0x7860)` = `0x003DB390` |
| `0x001F8DD0` | handle → descriptor slot |
| `0x001F8480` | transfer status poll (forced to 0; `TOL_VIF_STATUS_REAL=1` for real) |
| `0x001F8530` | transfer handler (DVCI open/read/copy/decompress) |
| `0x001F7390` | transfer yield-spinlock, `*(gp-0x7880)` = `0x003DB370` |
| `0x001F84B0` | worker 2 — transfer queue drainer (back edges overridden) |
| `0x001F8150` | worker 1 — GS worker loop (overridden: one iteration per dispatch) |
| `0x0015231C` | thread save/yield (**save-only**; also pumps DVCI, see §5) |
| `0x00151FF0` / `0x001520A0` / `0x00151E90` | guest scheduler entry / ready count / run loop |
| `0x00151780` / `0x001517C0` / `0x001517E0` | run-queue append / pop / subsystem init |
| `0x001518C0` | thread create + guest stack allocator |
| `0x0015243C` / `0x00152200` / `0x0015253C` | context save / restore helpers (restore = the open question) |
| `0x00198C60` | parked thread entry (current stall, `0x198C88`) |
| `0x00197940` | DVCI/ADXF pump (`DAT_00393208` = `0x00393208` guard) |
| `0x0019A080` | I/O wait → `FUN_001B7688(5)`; also pumps the transfer queue |
| `0x001F88F0` | script "finished?" predicate (called directly by thunk `0x1F7340`) |
| `0x003DB6BC` | resource table pointer (`gp-0x7534`) — **null in port**, `0x01523CA0` on HW |
| `0x003DB370` | transfer spinlock (`gp-0x7880`) |
| `0x003DB390` | transfer queue head (`gp-0x7860`) |
| `0x00394378` / `0x00394380` | thread park flags (current stall) |
| `0x00407160` | thread table (stride `0x210`) |
| `0x0048F590` | thread context array (size `0x1D0`) |
| `0x0048F570` | per-priority ready counters (`u16`) |
| `0x01523010` | boot script buffer — **empty in port**, `f6f6f6f6…` on HW |

Script handle: low 32 = command block pointer, high 32 = command index; `count = *(u32*)base`;
`status = !(index < count)`; a completed transfer writes `0xF6F6F6F6` at `base`, and `FUN_001f88c0`
then adds `0x10`.

---

## 8. Changed files

- `src/runtime/tol_overrides.cpp` — all runtime fixes and diagnostics.
- `thirdparty/PS2Recomp/ps2xRecomp/src/lib/control_flow_emitter.cpp` — jump-dispatch fix (durable).
- `data/output_fixed/` — 302 files patched locally (git-ignored).
- `PS2_PROJECT_STATE.md` — sessions 78–91.

---

## 9. Warnings for the next session

- `eeCheckpointDue()` is **time-based**. Single-run counters ("function X ran 0 vs 1 times") are not
  reliable evidence. Use `TOL_TRACE_SPIN` and longer windows.
- `[pc] exit <fn> pc=<other>` means a **checkpoint bail**, not a return. Reading it as `$ra`
  corruption wasted a full cycle.
- Two near-identical gp offsets: `4294937292` = `-0x7534` (resource table) vs `4294939820` = `-0x6B54`
  (unrelated). Verify against the generated C++, not the decompiler.
- `0x1F8150`'s interior `$ra` values (`0x1F8160`/`0x1F8168`) are **load-bearing** — they make the
  guest run its real loop body, which raises the completion flag. Removing them drops the flag to 0.
- **Do not build a second scheduler.** The guest has one (§5); extend it.
- Checkpoint-driven preemption interacts badly with anything driven from host callbacks: a bail
  unwinds to the runtime, and a host lambda cannot be resumed. Prefer letting guest code run.
- `pcsx2_*`: save state before continuing, restore afterwards (live user session).

---

## 0u. FIXED: the load stage now pumps, and the boot script lands

The gate is keyed on "the device exists and the pump is not inside itself" - deliberately NOT on the
handle/device state, because those flip 2 -> 0 -> 3 between ticks (measured) and any state test makes the
hook fire once. That mirrors the frame loop, which pumps unconditionally. The call is a **queued
invocation** (sp=0, ra=0, kind = Interrupt), not an inline call: the pump chain is emitted as direct C++
tail-calls that never touch $ra, so an inline call has no legitimate return address.

Measured, reproducible:

```
[load] submit #1 type=2 id=0 size=1252176 dest=0x01523010     <- the boot script, now completes
[load] resource id=1 ... slot=0x003DB6BC table=0x01523CA0 entry=0x00080000 -> 0x00020000
[load] resource id=2 ... entry=0x00080004 -> 0x00020001
[load] resource id=3 ... entry=0x00080008 -> 0x00020002
[load] submit #2 type=2 id=131072 size=530512 dest=0x005C64C0  <- next transfer starts

[script] status #1 base=0x01523020 index=0 count=5  readable=1
[script] status #2 base=0x015230A0 index=0 count=48 readable=1
[script] status #3 ... index=1 count=48
```

The handler chains through six windows (266, 255, 25, 39, 190, 128 blocks), the script dispatcher walks
real commands instead of spinning on an empty block, and the resource table at gp-0x7534 is populated -
the three symptoms that defined the original blocker are all gone.

**The two "next blockers" were not blockers.** JALR 0x3f800000 (an uninitialised sound-object vtable) and
the KORG sid=0x90000300 RPC timeout both still occur, but the load continues straight through them; they
were symptoms of a 24 s window that ended mid-transfer.

**Now default ON.** TOL_XFER_NO_TICK=1 reproduces the old stall for comparison. Verified default-on:
script lines 81, resources 1-3 loaded.

**Next:** the second transfer (530512 bytes) and the stages beyond it. The load is no longer stuck on the
boot script.
