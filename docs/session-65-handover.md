# Session 65 handover — Tales of Legendia native port

**Goal unchanged:** native PC port of *Tales of Legendia* (SLUS_212.01) via PS2Recomp. Not an emulator.
Build: `build/tol2` (clang-cl + Ninja), run `build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu`.

---

## 1. Where the port stands

Unchanged from session 64 at the milestone level (renders NOW LOADING, 201 draws, loading never advances), but the
stall is now **named to the instruction** instead of being attributed to "threads".

| milestone | state |
|---|---|
| Recompile + launch, 0 dispatch errors | done |
| IOP image + 9 IRX + IOPRP services, DVCI indexes 6 AFS, audio init, ioman file layer | done |
| Graphics | renders 201 draws / 81 GIF packets (512x448 "NOW LOADING") |
| **Loading sequence advances** | **blocked — root cause now precise, see §2** |
| Guest threads | still only 1 (CreateThread 0x20 never called) — **not shown to be the blocker** |

## 2. The blocker, precisely

The loading sequence is `FUN_001664d0`; it starts six AFS loads with `FUN_00196058(i, path, 0, buf)` and waits with

```c
while (FUN_00196460(i) != 3) FUN_0019a080();   // FUN_00196460 = ADXF_GetPtStat, FUN_0019a080 = CRI pump
```

Measured with the new `TOL_TRACE_DVCI=1` probe (wraps `FUN_00196460` and `FUN_00195A18`):

```
[dvci] error print ra=0x00196810 text='E0040701:Illigal format(not AFS).(ADXF_GetPtStat)'
[dvci] #1 poll(i=1) -> 49 | pc=0x0010EE5C ra=0x0010D948 | load=0 handle=0x00390738 buf=0x00391140 blocks=1
[dvci] error print ra=0x001964D0 text='E0041303:illigal parameter 'ptid'.(ADXF_GetPtStat)'   (every later poll)
```

So the first poll **already fails the container check**: the AFS header never landed in the request buffer.

The check is `memcmp(buf, "AFS", 3)` inside `FUN_00196460` (`FUN_00108EB8` is memcmp; the 3 bytes at `0x3C1AB0` are
`"AFS"`). On mismatch it goes to `LAB_00196808` → `FUN_00195A18("E0040701…")` → `FUN_001963A8()` (abort) → return 4.

**Second defect on top of it — the error path destroys the caller's state.** The poll's failure branch calls the
guest's message printer `FUN_00195A18`, which runs the guest's own printf core `FUN_0010D850`. A *non-call* dispatch
inside it (`dispatchGuestBranch` returns false for every `j`/`b`) unwinds the generated C++ chain, so the poll returns
with `pc = 0x0010EE5C` (still inside printf) and `v0` = leftover garbage (`49`, then `50` = ASCII '1'/'2' from the
printf internals). The caller then bails too, and `FUN_001664d0` is re-entered from the top each iteration. That is
why the state machine's bookkeeping is lost and why the loop can never observe state 3 even if a later poll would
produce it.

## 3. Measured facts about the CRI/ADXF machinery (all in our port)

- The read *is* submitted: `FUN_00196058` → `FUN_001961a8` → `FUN_001970e8` → `FUN_00196f58` → `FUN_001b5118`
  (allocates an SJRBF request in the pool at `0x003B1998`, callback `FUN_001B5050`) → `FUN_001B5260`.
- The CRI machinery runs: `FUN_0019ED68` (kick reader) ×1, `FUN_0019F468` (per-stream pump) ×56,
  `FUN_0019F298` (one stream step) ×10, `FUN_0019ED60` (reader status) ×2.
- The ADXF reader slot is `0x0039EE48` (`= *(handle + 4)`, slot 16 of the 40×0x60 table at `0x0039E848`):
  `active=1`, `+0x00 = 0x00000101`, `+0x08 = 0x00499DC0` (its own buffer), `+0x2c = 0x200`.
- At poll time the SJRBF request pool `0x003B1998` is **empty** (request consumed) and `DVCI load = -1` (idle).
- `FUN_0019F468`'s gate `DAT_0039E838` reads 0, which is the *normal idle* state — `FUN_001B79F8` is an
  acquire/release busy flag, not a "disabled" flag. Not a bug.
- The CRI device table `0x003A7FB0` (32 × 0x238, live when byte 0 == 1) is **entirely zero** in our port.

### Where the bytes do NOT come from (all checked, all negative)
- No `[fileio]` event of any kind for the AFS read: game-file RPC (sid `0x80000597`), ioman RPC (`0x80000001`),
  IOP file layer (`TOL_TRACE_IOP_FILEIO`), and no unknown-fd ioman read.
- No `[sceCdRead]` log line at all, and launching with `--iso=data\Tales of Legendia (USA).iso` changes nothing.
- CDVD RPC (`sid 0x80000592/0x80000593`) is called only twice, before any file is opened.

So the ADXF reader completes its request **without transferring a byte**, and the transfer point is still unidentified.
`FUN_00196db0` is where a read is started (`FUN_0019f648` / `FUN_0019f548` / `FUN_0019f570` / `FUN_0019ee18` on the
reader), and `FUN_0019eec8` is its blocking wait (`do { FUN_00197940(); } while (state != 1)`); `FUN_00197940` is the
CRI server pump calling `FUN_0019f468`, `FUN_001a9a08`, `FUN_001ab8a8`, `FUN_001973a8`.

## 4. Hypotheses eliminated this session (do not re-chase)

| Hypothesis | Why it is dead |
|---|---|
| Checkpoint preemption loses the poll's return value | `TOL_TRACE_CKPT=1`: zero declines |
| A missing map entry / dispatch hole | 0 missing-targets in a clean run |
| The pump callback table (0x49C6A0 slot 5) is incomplete | identical to PCSX2: only `0x00198210` |
| PCSX2's live device slot at 0x3A7FB0 is the AFS device | it is **ADXT** audio streaming: its `+0x14/+0x18 = 0x2F999/0x3B800` is the 0.85 ratio from `FUN_0019fc80` (`ADXT_Create` → `FUN_001b0870`), not AFS |
| ioman/gamefile fd tables are split, so the AFS read hits an unknown fd | no read ever arrives with an unissued fd |
| DVCI reads raw sectors via `sceCdRead` (and our stub zeroes the buffer) | `sceCdRead` is never called; `--iso=` changes nothing |
| The missing piece is `CreateThread` (session 64's theory) | still true that no thread is created, but PCSX2 also pumps the loading sequence on the **main** thread (`backtrace`: `FUN_001664d0 → FUN_001b768c → FUN_00198210 → FUN_001b0d00`), so threads are not what advances this loop |

## 5. Next steps, in order

1. **Find the ADXF transfer point.** Wrap the reader's device object (`0x0039EE48`, its vtable at `+0x00`/`+0x08`) and
   the request completion callback `FUN_001B5050`; whoever should fill the request buffer is the missing link.
   `FUN_0019F298` (one stream step, entered 10×) is the most likely caller.
2. **A/B the reader object against PCSX2 at the same point.** PCSX2 is at a post-loading steady state (8 threads,
   TID0 parked at `0x81FC0`), so it holds the *healthy* shape of `0x0039E448..0x0039EEA8`:
   `pcsx2_read_memory(0x39EE48)` and compare `+0x08` (buffer), the vtable and the device pointer against ours.
3. **Consider the 0-byte completion before the plumbing.** A request that completes with no error and no data
   usually means the device object's read callback is a stub returning 0 bytes — check the reader's vtable entries.
4. **The unwinding error path is a real robustness hole** worth fixing eventually: one failed read currently costs the
   whole guest call chain (the poll and `FUN_001664d0` restart from the top). Any fix must respect that the
   dispatch-every-jump model is what DQ8 ships with.

## 6. New tooling added this session

| env | what it prints |
|---|---|
| `TOL_TRACE_DVCI=1` | wraps `FUN_00196460` (`ADXF_GetPtStat`) + `FUN_00195A18` (error printer, prints its message text); dumps the DVCI state block (`0x3910A4..0x3910B4`), the ADXF reader slot, the CRI device table `0x3A7FB0`, the request pool `0x3B1998`, and the pump gate `0x39E838` |
| `TOL_TRACE_EXC=1` | in `ps2_runtime.cpp`: exception code + pc/ra/sp/v0/v1/a0, plus `[exc-mem] <load|store> addr=… pc=…` naming the rejected address |
| `[fileio] ioman read fd=… -> N` / `-> unknown fd` | new: ioman read outcomes (previously silent) |

Runtime-side additions live in `thirdparty/PS2Recomp` (`ps2_runtime.cpp`, `raiseCop0Exception` +
`traceRejectedAccess` + the 10 Load/Store catch blocks).

## 7. Land mines learned this session

- **Never `Load32` an unaligned guest address from a probe.** `PS2Memory::read32` throws on `addr & 3`, `Load32`
  turns that into a genuine guest `EXCEPTION_ADDRESS_ERROR`, and the runtime vectors the guest to `0x80000080`
  (`EXCEPTION_VECTOR_GENERAL`) which is **not recompiled** — so a one-byte probe mistake destroys the whole call
  chain and looks exactly like a game bug. My own `buffer + 0x45` dump did this. Use `Load8` for unaligned fields.
- `loadScalar`/`read32` also throw on TLB misses and out-of-range, so `[exc-mem]` must be read before assuming
  "unaligned".
- The decompiler's `FUN_00196460` shape is misleading: the real exit is `li v0, 4` (error) or
  `lb s6,1(v0); move v0,s6` (state), at `0x196818` / `0x196820`; everything after `0x196854` belongs to a different
  function that the map folded into the same unit.
- Global addresses in `.data` must be taken from the disassembly (`lui v0,0x39; lw v1,0x10ac(v0)`), not from a
  hand-typed constant: I dropped a hex digit (`0x00390AC` vs `0x003910AC`) and read zeros for ten minutes.
- `TOL_TRACE_PC` only patches addresses that exist in the function table; an interior address silently fails to
  install (it prints "is not in the function table"), so an absent `enter` line is not proof a function did not run.

## 8. Environment notes

- PCSX2 is connected and is the reference. It crashed once during a resume/pause cycle mid-investigation; it came
  back by itself. Avoid rapid pause/resume churn and `get_modules`, which hung once before.
- Breakpoints used successfully: `0x1b0870` (device registration) → backtrace showed `ADXT_Create`. A breakpoint at
  `0x196460` did **not** hit in the post-loading idle state (the game is not reading AFS containers while idle).
- PCSX2 is currently paused at the EE kernel park loop (`0x00081FC0`) with 8 threads, i.e. **after** loading.

## 9. Session 65, second pass — the DVCI partition is opened with size 0, so no read is ever issued

The blocker is now traced to `handle[+0x0C] == 0` (the partition's sector count). Everything follows from that:

```
FUN_00196058(i, path, 0, record)                     // DVCI "start a partition load"
  └ FUN_001961a8                                     // record[0x390B78 + i*4] = rec
      ├ FUN_00197738(i, 0, rec+4, &size, &off, &sectors)
      │   └ FUN_00197698(i, 0)  ->  -3 when 0 >= rec+8 (entry count)
      │   so size = rec+0x110 = 0, sectors = 0
      └ FUN_00196a10(fd, size=0)                     // cache-miss open by fd
          └ FUN_00196960(handle, fd, 0)
              ├ FUN_0019ec90(stream, fd, 0, 0, 0xFFFFF)   // stream window = 0 sectors
              └ handle[+0xC] = FUN_0019f568(stream) = 0   // <-- nothing to read
```

Then `FUN_00196db0` computes `iVar1 = handle[+0xC] - handle[+0x14] == 0` and takes the
`state = 3 (DONE)` branch **without issuing any read**, which is why the packet "completes" with no bytes and the
poll's `memcmp(buf, "AFS", 3)` fails.

The chain is circular on purpose-built data: `rec[+8]` (count) and `rec[+0x110]` (size) are written **by the poll**
after it parses the header block, and the header block only arrives if a read is issued. Our port never read that block,
so the record stays all zeros (verified: `rec = 0x01AF9580`, everything 0 except `type=1` at +0xF and the name at +0x10).

### Ground truth captured from PCSX2

- DVCI globals while idle: `0x3910A0=6`, `0x3910A4=0` (progress), `0x3910A8=0` (handle closed),
  `0x3910AC=0xFFFFFFFF` (no ptid in flight), `0x3910B0=0x00391140` (data buffer — **same address as ours**),
  `0x3910B4=0`.
- The data buffer `0x00391140` holds a real AFS header once a load has worked:

  ```
  41 46 53 00 | 16 00 00 00 | 00 08 00 00 | 00 b0 99 03 | 00 b8 99 03 | 00 58 07 03 | 00 10 a1 06 ...
  "AFS\0"       count = 22    0x800         size0        off1          size1         off2
  ```

  i.e. **standard little-endian AFS**: magic, `u32 count`, then `count` x (`u32 offset`, `u32 size`), offsets aligned
  to 2048. This is exactly what a correct read must leave in `0x00391140`.
- `data/CD/AFS/FLIST.DIR` is only a CRLF-separated path list (`\AFS\SYS_REG.AFS`, `\AFS\FIELD.AFS`, …) — **no sizes**.
  So the archive size can only come from the AFS header, which confirms the bootstrap must not need a size.

### Tried and disproved

- Publishing the host file size in the game-file open reply at `+0x08/+0x10/+0x18/+0x20` (the words `FUN_00101a60`
  copies into its 5-word result struct): **no change** — the DVCI stream size is not read from there. Reverted.
- `--iso=` and `TOL_TRACE_IOP_FILEIO`: no sector read anywhere; the AFS handle is never used for I/O (no ioman
  lseek/read on it, no game-file read, no `sceCdRead`).

## 10. Session 65, third pass — the size is in SECTORS; supplying it makes the port really load AFS data

### The unit mistake that blocked everything

The CRI file device counts in **2048-byte sectors**, not bytes:

- device `seek` `FUN_001AD780`: `handle[+0x0C] = handle[+0x08] + offset` for `SEEK_END`
- device `tell` `FUN_001AD818`: returns `handle[+0x0C]`
- `FUN_0019f298` stores that value straight into the ADXF stream's sector count `stream[+0x14]`, and the byte window
  is `stream[+0x10] = count << 11`
- `FUN_00196960` copies it into the DVCI handle's `+0x0C`, which `FUN_00196db0` subtracts from `+0x14`

Patching a *byte* count there makes the byte window overflow (`stream[+0x10] = 0xDF800000`) and still nothing reads.

### With the size supplied in sectors, the load actually runs

`TOL_DVCI_SIZE_SHIM=1` (wraps `FUN_001AD780`) answers `seek(END)` from the size of the file the guest just opened
through its own file server. Measured effect:

```
[dvci] seek(END) handle=0x003A75F8 -> 894 sectors (1830912 bytes) patched
[fileio] gamefile read handle 8 2048/2048 bytes at 0x00391140        <-- the AFS header really lands
[dvci] #1  poll -> 2   progress=0
[dvci] #20000 poll -> 2 progress=255                                  <-- 255 extents parsed (one block's worth)
```

- the `E0040701: Illigal format(not AFS)` error is **gone**;
- the poll returns **2** (read in progress) and returns *normally* to the loading loop (`pc=0x00166780`), so the
  stack unwinding described in §2 no longer happens;
- `SYS_REG.AFS` has `count = 0x1AE = 430` entries, i.e. `8 + 430*8 = 3448` bytes of header+table = **2 sectors**;
- only one block is read because `DAT_003910B4` (blocks) = 1, and one 2048-byte block holds 255 eight-byte entries.

### The remaining gap, exactly

`FUN_0019F298`/`FUN_0019EF58` stop at the block boundary; the poll parses 255 entries, calls `FUN_001970E8` to read
the **second** block, returns 2, and every later poll returns 2 straight away (`FUN_00197668(handle) == 2`), so the
parse never continues.

The first block completed because `FUN_00196DB0`'s own blocking wait (`FUN_0019EEC8` → `FUN_00197940`, the CRI server
pump) pumped it **inline**. The second read is issued without a blocking wait, so it depends on the SJRBF pump
`FUN_001B0D00` walking the device table at `0x003A7FB0` — and that table is **all zero**, because `FUN_001B0870` (the
only slot registrar) is never called for this stream.

So the next piece to build is one of:

1. the device-slot registration for the ADXF/SJRBF stream (what `FUN_001B0870` does: `slot[0]=1`, `slot[8]=pool`,
   `slot[0x18]=iVar1+iVar2`, `slot[0x14]=0.85*that`, and `FUN_001B09C8(slot, ...)`), or
2. a native equivalent of "pump the stream once per poll", i.e. call the CRI server pump (`FUN_00197940`) from the
   loading loop, which is what the blocking-wait path already does successfully for block 0.

### Device chain, confirmed at runtime

`reader(0x39EE48) + 8` → file object `0x00499DC0` → device `0x003A6A98`, handle `0x003A75F8`.

```
dev slots: +00=001AD2E8 +04=001AD360 +08=001AD478 +0C=00000000 +10=001AD5A0(open) +14=001AD728
           +18=001AD780(seek) +1C=001AD818(tell) +20=001AD850(read) +24=00000000 +28=001AD9F8 +2C=001ADAF8
```

All slots are populated, so the device is complete — only the size it is given was wrong/absent.


## 11. Session 65, fourth pass — threads work; the gap is DVCI request retirement

### Correction: `CreateThread` is not a syscall in this corpus

The recompiler bound it as a stub, so it is a direct C++ call and never appears in a syscall trace:

```
CreateThread_0x1144e0.cpp:   ps2_syscalls::CreateThread(rdram, ctx, runtime);
register_functions.cpp:      table[20790] = CreateThread_0x1144e0; // 0x1144e0
```

An earlier conclusion in this session ("the guest issues zero thread syscalls") was measuring the wrong thing. Same
applies to `StartThread`: check `data/output_fixed/<Name>_0x*.cpp` for a stub before concluding the guest never calls
an API.

### Threads are implemented AND exercised — nothing to extend

| probe | result |
|---|---|
| `FUN_001997a8` (ADX/middleware init) | ran |
| `FUN_00199428` builds `ee_thread_t{entry=FUN_00198d50, stack=0x00395BF0, size=0x1000}` then `CreateThread` + StartThread + `ChangeThreadPriority` | ran |
| `FUN_00198d50` worker body (`while (...) { ... FUN_001b7828(); ... }`, ends in `ExitDeleteThread()`) | **running** |
| `FUN_001b7858` -> `FUN_001b7688(4)` (slot-4 pump, inside the worker loop) | 17x in 8 s |
| `FUN_00198280` -> `FUN_00197940` (CRI server pump) | 15x in 8 s |

So `EeScheduler` really switches to a second guest thread, and the threaded pump registration in `FUN_001982a0`
(slot 4 = `0x198280`) is correct for this title. The single-threaded topology shim is NOT needed.

### The read chain now works end to end (with the size shim)

`[adxf]` stream-step trace (`TOL_TRACE_DVCI=1`):

```
step #1  state=1 45=1                        -> state=1 49=1        open request consumed
step #2  state=1 +14=894 sectors             -> unchanged           window from tell(END)
step #3  state=2 47=1 +1C=800 +40=800        -> +20=1 +24=0x391140 +28=0x800   read armed
step #4  state=2                             -> state=3 +58=1       read completed
```

Zero error prints: the block really is read and the stream really completes.

### Still wrong: the DVCI handle never retires the request

The stream reaches 3, but the **DVCI handle stays at state 2** and its consumed counter `handle[+0x14]` stays 0.
`FUN_00196460` only parses while `FUN_00197668(handle) == 3`, so it returns 2 forever, `DAT_003910A4` freezes at 255
(one block of the 430-entry table) and the stream re-reads block 1 in a loop (steps #38..#40 repeat with
`+20=1 +24=0x391140 +28=0x800`).

The missing link is between stream completion and the DVCI handle: the SJRBF request is not retired and
`handle[+0x14]` is never advanced. Instrument next: the request completion callback `FUN_001B5050`,
`FUN_0019EE18(stream, handle[+0x1c])`, and `FUN_00196F58`'s update of `handle[+0x28]`/`handle[+0x14]`.

Because our file RPCs complete **synchronously**, the likely shape is an ordering race: the completion runs before the
handle has latched the request, so nothing credits the bytes read.

### Follow-up: the credit path exists and runs; the parse still never resumes

- `FUN_001973A8` is the DVCI progress tick: it walks 16 handles at `0x00390738` (stride `0x44`) and calls the credit
  routine `FUN_001972E0` for every handle whose byte 0 is 1. Only caller: `FUN_00197940` (the CRI server pump), so the
  guest worker thread drives it — 35 calls in 12 s.
- `FUN_001972E0` is the credit routine: it copies the stream state into `handle[+0x01]` and, when the new state is 3
  or 4, does `handle[+0x14] += (streamBytes - handle[+0x14])` and calls `FUN_00196C28(handle)`.
- Measured from inside the tick: the handle is active on **every** tick (`b0=1`) and its state cycles
  `1 -> 2 -> 3 -> 2 -> 2 ...`, so the credit path is running.
- Yet `DAT_003910A4` (parse progress) stays at **255** and `rec[+8]` (entry count) stays **0**, i.e. `FUN_00196460`
  never executes the header/parse branch, so the 430-entry table never completes and the stream re-reads block 1.

Next measurement: probe `FUN_00196460` and print at **entry** `handle[+1]`, `rec[+8]`, `DAT_003910A4`, `DAT_003910AC`,
and count how often it enters with `handle[+1] == 3`. If that never happens the state-3 windows never overlap the poll;
if it does, the parse branch is failing silently.

### Follow-up 2: the parse DOES run — the state simply never returns to 3

Poll-entry probe (`[poll-in]`, printed before the original runs):

```
[poll-in] #1     i=0 ptid=0 state=2 count=0 progress=0
...
[poll-in] #3370  i=0 ptid=0 state=3 count=0 progress=0   <-- the only state-3 entry in 148k polls
[poll-in] later  state=2 for every remaining entry
```

and the last poll of the run reports `progress=255`. Since the copy loop only advances while
`rec[+8] > DAT_003910A4`, reaching 255 proves the parse branch ran, that `memcmp(buf,"AFS",3)` **passed** and that the
entry count was written (430). **The "not AFS" failure is fully solved by the size shim**; zero error prints in the
whole run.

So a single question is left: **why does the handle's state byte come back to 3 only once?**

The state is only refreshed by the tick's credit routine `FUN_001972E0`, whose guard is `handle[+1] == 2`; after
crediting it calls `FUN_00196C28(handle)`, and the state it adopts is whatever `FUN_0019ED60(stream)` returns. The
stream cycles 2/3 (see the `[adxf]` trace), yet after the first credit the handle stays 2 for the rest of the run, so
either `FUN_00196C28` clears the precondition or `FUN_0019ED60` reports 1 (idle) at the moment the tick samples it.

Next: dump `FUN_0019ED60(stream)` and `handle[+1]` around one tick, plus what `FUN_00196C28` writes - that closes it.

### Follow-up 3: the tick samples the stream one pump too late (synchronous-completion race)

- `FUN_00196C28(handle)` only **retires the SJRBF request** (`handle[+8]` → `(*(vtbl+0xc))(request)`); it does not
  touch `handle[+1]`.
- `FUN_0019ED60(stream)` is just `return stream[+1]` — the ADXF stream state.
- So the credit routine copies the stream state verbatim: `handle[+1] = stream[+1]`.
- `FUN_00197940` pumps in this order: `FUN_0019f468(); … FUN_0019f468(); FUN_001973a8();` — the tick runs **after two
  stream pumps**. Our file RPC completes instantly, so a read goes 2 → 3 → (re-armed) 2 inside that same cycle, and
  the tick almost never observes 3. Measured: exactly **1** state-3 observation in 148k polls.

On hardware the read takes milliseconds, so state 3 persists across many pump cycles and the tick always catches it.
This is the same class as the size bug: our synchronous I/O erases the window the guest's state machine needs.

Fix to try next: give async reads one pump of latency — wrap the device status slot (`0x001ADAF8`) so it reports
"busy" once before "done". That should let the tick observe 3, credit `handle[+0x14]`, and the parse continue
255 → 430 → poll returns 3.

### Follow-up 4: the one-poll latency shim changed nothing

`TOL_CRI_IO_LATENCY=1` wraps the device status slot `FUN_001ADAF8` (`return deviceHandle[+2]`) so a completed read
reports busy once before done. Result: unchanged - still exactly **1** state-3 observation and `progress` still freezes
at 255.

So the "tick samples one pump too late" theory is **not confirmed**. Reading `FUN_0019EF58`'s completion path again,
the stream only sets `stream[+1] = 3` when its whole window is consumed (`iVar4 >= iVar2`); until then it keeps
arming the next block and stays at 2. So the question is what the stream's `iVar2` (total) is versus `iVar4` (done) at
the moment the tick samples, not the sampling order.

Next: log `FUN_0019ED60(stream)` from inside the tick together with `stream[+0x34]`/`stream[+0x58]` (done/total
byte counters) so the picture is unambiguous before touching anything else.

### Follow-up 5: per-request window makes the loading loop advance through all six archives

`TOL_CRI_WINDOW_FIX=1` wraps `FUN_0019EE18(stream, transferSectors)` - the last call of `FUN_00196DB0`'s arming
sequence - and sets the stream window from that request: `stream[+0x14] = sectors`, `stream[+0x58] = 0`,
`stream[+0x10] = sectors << 11`.

Measured (with `TOL_TRACE_DVCI=1`):

```
[dvci] start(i=0) .. [dvci] start(i=5)          <- all six AFS loads started
DVCI: File cache was not hit. SYS_REG / MAP / SND / FIELD / BATTLE / MOVIE   <- all six, in load order
progress reached 1279 (was frozen at 255)
loading loop reaches load index 5; screen still 201 draws
```

So `FUN_001664d0`'s six-load loop now runs to its last archive instead of dying on the first. **Caveat:** without the
`TOL_TRACE_DVCI` probes the same build still stalls at load 0 (only one cache-miss line), so the result is
timing-sensitive - the probe overhead changes the interleaving, which means at least one more race remains between the
synchronous file completion and the worker's pump cadence.

Next: make the window fix independent of timing (the remaining race is almost certainly the same completion-before-arm
ordering), then confirm load 5 completes and `FUN_001664d0` returns.

### Follow-up 6: the "timing sensitivity" was an install-gating bug (fixed)

Both `TOL_DVCI_SIZE_SHIM` and `TOL_CRI_WINDOW_FIX` were installed **inside** `if (dvciTraceEnabled())`, so with tracing
off they were never installed at all. Moved them out; each is now gated only by its own variable.

With tracing completely off (`TOL_DVCI_SIZE_SHIM=1 TOL_CRI_WINDOW_FIX=1` only):

```
[dvci] size shim installed on 0x001AD780
[dvci] per-request window installed on 0x0019EE18
DVCI: File cache was not hit  x6      <- SYS_REG, MAP, SND, FIELD, BATTLE, MOVIE
```

So the per-request window is deterministic: all six archives load again and again, no probes needed.

### Where it stops now

- The guest spins at `pc=0x1b0d28 ra=0x198220` = `FUN_001b0d00` under `FUN_00198210`, i.e. the loading loop's own SJRBF
  pump - it is sitting on the **last** archive (index 5), whose parse never starts (`progress=0`).
- New symptom, first time seen: `[ps2xIOP:warning] [iop] RPC to 2415919872 timed out` = **0x90000200** (IxAudio). The EE
  is now issuing IOP RPCs that nothing answers, so after the six archive loads the next wall is the IOP RPC path, not
  the AFS one.
- Screen unchanged: still 201 draws / 81 GIF packets. No logos yet.

### Follow-up 7: loads 0-4 complete, load 5 never reaches state 3

With the install gating fixed and no tracing:

- All six archives read their extent tables block by block: game-file handles 8..13 -> 2, 4, 31, 8, 22 and 1 reads of
  2048 bytes. The parse ran (poll `count` non-zero on 246,605 entries; state 3 observed 67 times).
- Load index **5 alone** never reaches state 3: every poll logs `state=2 count=0 progress=0`, so its parse never
  starts. Nothing general is wrong any more - it is one handle/stream.
- The `[ps2xIOP] RPC to 2415919872 timed out` warning is **IOP-side** (a module on the emulated IOP calling
  0x90000200 with no provider). Our EE-side handler already answers every function of that sid (unknown ones reply 0),
  so it is a separate, later wall - not the loading stall.

Next for load 5: log whether the arm happened (`handle[+0x1c]`, and whether it is 0 - the window probe returns early on
0 sectors) and whether `handle[+0x0C]` is non-zero, i.e. whether the open for the sixth archive actually produced a
size.

### Follow-up 8: window applies correctly; the wall moved to the IOP audio RPC

Tracing every arm (`[dvci] window #n`) shows all requests use the **same** stream object `0x0039EE48`, and each arm
correctly writes `total=1 done=0 bytes=0x800`. Archive 6's handle fields are also correct
(`total=436462` = MOVIE.AFS exactly, `transfer=1`). So there is no object mismatch and no sizing error left in the
DVCI/ADXF path.

Adding `TOL_CRI_IO_LATENCY=1` on top of the size shim + window fix (all three installed outside the trace gate,
tracing off) changes nothing: six archives still load, screen still 201 draws, and the last thing the guest does is
spin while the IOP reports repeated

```
[ps2xIOP:warning] [iop] RPC to 2415919872 timed out      # 0x90000200 = IxAudio
```

Conclusion: the AFS/DVCI work is effectively done; the current blocker is the **IOP-side IxAudio RPC (0x90000200)**
having no registered server, not the loading loop.
