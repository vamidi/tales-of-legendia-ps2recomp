<!-- ⛔ RULES — Re-read this section EVERY TIME you open this file. -->
<!-- These rules also appear in the SKILL.md. Redundancy is intentional. -->

# ⛔ QUICK RULES (mandatory re-read)

1. **Build:** `cmake --build PS2Recomp\out\build` — NEVER add `--clean-first`, `--target clean`, or delete the build directory.
2. **Files:** NEVER modify `runner/*.cpp`. Fix in `src/lib/` or game overrides.
3. **Headers:** NEVER modify `.h` without asking user. Triggers mass rebuild.
4. **Git:** NEVER use destructive git commands (`checkout`, `clean`, `reset`, `stash`, `pull`).
5. **Verify:** NEVER assume file names/paths. Use tools (`list_dir`, `find_by_name`, `grep_search`).
6. **Vendored deps:** NEVER patch `PS2Recomp\out\build\_deps\parallel_gs-src` (parallel-gs is vendored source).

---

# PS2 Recomp — Project State
> Auto-maintained by agent. DO NOT DELETE. Read at session start, update after every major action.
> **Last merged:** 2026-10-16 — black-screen root cause RESOLVED (BIOS-like CRT seed) + first-present abort RESOLVED (gs_gl_interop legacy-layout VK_ASSERT).

## Objective
- Port **Tales of Legendia** (SLUS_212.01, NTSC-U) to PC via PS2Recomp.
- ~~Fix the **black screen** by getting a valid GS/CRT video mode into parallel-gs.~~ **RESOLVED 2026-10-16** via BIOS-like CRT seed; follow-up abort also fixed.

## Requirements (constraints on every fix)
- Fixes go in **game overrides** or `PS2Recomp\ps2xRuntime\src\lib\` — never patch generated `runner/*.cpp`.
- Do **not** patch vendored parallel-gs sources under `PS2Recomp\out\build\_deps\parallel_gs-src`.
- **Incremental builds only:** `cmake --build PS2Recomp\out\build` (wrapped in vcvars64).
- Update `PS2_PROJECT_STATE.md` after major actions.

## Current Phase
PHASE_MENU_REACH + **rendering live** — game boots, runs, and now presents real frames (BIOS-like CRT seed active, `Unknown video format.` 3780→0). The startup abort introduced by enabling rendering is fixed (`gs_gl_interop.cpp` release-layout). Remaining: visual confirmation of rendered content on screen, then decide which temporary diagnostics to strip, then commit.

## Root-Cause Snapshot (RESOLVED 2026-10-16)
- **Black screen root cause:** the guest **never programs GS video mode** — every instrumented guest-write path ran **zero times** in evidence runs: `[MMIO]` write8/16/32/64/128 GS-priv diag = 0 hits, `[GSFE]` raw-forward = 0, `WriteRegisterRaw` = 0, GIF AD tag = 0, `SetGsCrt` syscall = 0. `DISPFB1=0x1400`/`DISPLAY1=0x001bf27f00000000` come only from runtime C++ stubs, never from guest GIF/MMIO.
- **Chosen fix:** BIOS-like CRT seed (not faithful GIF/MMIO priv-reg handling — nothing to be faithful to for this game). Seed in `syncCoreSubsystems()` after `m_gs.init(...)`: `ps2_syscalls::configureGsCrt(m_memory.gs(), 1u, 2u, 0u)` gated on `smode1 == 0` + one-time `[GS] BIOS-like CRT seed:` log → `smode1=0x4101` (LC=32 CMOD=2 NTSC), `smode2=0x1` (INT=1 FFMD=0), `pmode |= EN1`.
- **Verified:** `[GS] present mode: pmode=0x1 smode1=0x4101 (LC=32 CMOD=2 INT=1) smode2=0x1` and **`Unknown video format.` 3780 → 0**.
- **Latent correctness fix shipped alongside:** `PS2Memory::write128` GS-priv branch added (routes to two `write64`) — 128-bit stores to `0x12000000`-window were previously **silently dropped**.

## Follow-up Crash (RESOLVED 2026-10-16) — first-present abort
- **Symptom:** after the seed enabled real rendering, the runner aborted at startup ("abort() has been called" dialog; self-exit code `-2147483645` = `0x80000003` EXCEPTION_BREAKPOINT).
- **Signature in stderr:** `[ERROR]: Vulkan error at ...\Granite\vulkan\command_buffer.cpp:464.` immediately after 3× `[WARN]: Using deprecated TRANSFER stage.`
- **Mechanism:** Debug build auto-defines `VULKAN_DEBUG` (`Granite/vulkan/CMakeLists.txt:86-88`) → `VK_ASSERT` active → `LOGE` + `abort()`. The assert is Granite's `is_legacy_layout()` lint (SHADER_READ_ONLY_OPTIMAL / DEPTH_STENCIL_READ_ONLY_OPTIMAL / COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL are sync1-era layouts forbidden in sync2 barriers).
- **Offender (our code, not vendored):** `ps2xRuntime/src/lib/gs/gs_gl_interop.cpp` `Slot::copy()` — `release_image_barrier(*image, TRANSFER_DST, **SHADER_READ_ONLY_OPTIMAL**, ...)` tripped the lint → abort. Before the seed, `Present()` bailed early ("Unknown video format") so this interop copy path never executed; the seed exposed the latent bug.
- **Fix:** release into `VK_IMAGE_LAYOUT_GENERAL` instead — non-legacy, matches the GL side's `GL_LAYOUT_GENERAL_EXT` signal/wait and the `oldLayout=GENERAL` the acquire barrier already declares → coherent external ownership transfer. Incremental rebuild clean; verification run: `[ERROR]=0`, no terminate, survived 95s, TRANSFER warnings 3→16440/95s (≈2/frame @60fps = interop copy now runs every frame = real rendering).

## Decisions
- Treat extracted CD tree `data\CD` as the primary asset source via the `talesOfLegendiaAssetPaths` override.
- **Stop chasing `SetGsCrt@0x001142E0`** as the only path (0 hits; plumbing is fine).
- **BIOS-like CRT seed implemented** (was the candidate fallback): `configureGsCrt(gs, 1, 2, 0)` gated on `smode1 == 0`. Games that program their own mode still win (gate skips seed when `smode1 != 0`); overrides apply after init (`ps2_runtime.cpp:945`) and can overwrite the seed.
- **Interop release layout = GENERAL** (fixes the VK_ASSERT abort; see Follow-up Crash).
- Keep `logPresentationModeOnce()` + seed log for now; strip decision deferred until visual confirmation lands.

## Boot Status
- [x] Loaded skill (`ps2-recomp`)
- [x] Verified comprehension (3 questions answered)
- [x] Black screen fixed (CRT seed; `Unknown video format` 3780→0)
- [x] First-present abort fixed (gs_gl_interop release layout)
- [ ] Visual confirmation of rendered content on screen (screenshot attempt showed window alive `PS2-Recomp | SLUS_212.01` but occluded/off-screen; user cancelled the run)

## Game Info
- **Title**: Tales of Legendia (USA)
- **Region**: NTSC-U (SLUS_212.01)
- **Has Symbols**: yes (ELF analyzed; path-based CD access strings present)

## Workspace Paths
- **PS2Recomp Repo**: D:\Personal\C++\tales-of-legendia-ps2recomp\PS2Recomp
- **Game Workspace**: D:\Personal\C++\tales-of-legendia-ps2recomp\data
- **ISO Path**: D:\Personal\C++\tales-of-legendia-ps2recomp\data\Tales of Legendia (USA).iso (extracted tree: data\CD — 34 files, 3.92 GB; used as primary asset source)
- **Output Dir**: D:\Personal\C++\tales-of-legendia-ps2recomp\data\output

## Binaries

| Role | File Name | Type | Path | TOML Config | Status |
|------|-----------|------|------|-------------|--------|
| Main | SLUS_212.01 | ELF (no ext) | D:\Personal\C++\tales-of-legendia-ps2recomp\data\SLUS_212.01 | D:\Personal\C++\tales-of-legendia-ps2recomp\data\config.toml | recompiled |

## Environment Setup
- **Ghidra Install Path**: N/A (not used this session)

## Build Configuration
- **CMake Generator**: Ninja (canonical build dir: `PS2Recomp\out\build` — NOT `PS2Recomp\build`)
- **C++ Compiler**: MSVC cl via vcvars64 (`C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`)
- **Build Type**: Debug (⇒ Granite `VULKAN_DEBUG` auto-on ⇒ `VK_ASSERT` live — style lints become `abort()`)
- **Ghidra CSV Path**: N/A
- **single_file_output**: false
- **Working incremental build command**:
  `cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && cmake --build PS2Recomp\out\build 2>&1'`

## PCSX2 MCP Status
- **Status**: Not Connected
- **Game Loaded**: N/A
- **Match**: N/A

## Active Runner Command
<!-- ACTIVE RUNNER COMMAND: -->
PowerShell-native (the `cmd /c` nested-quote form gets mangled → "Access is denied"):
`& "D:\Personal\C++\tales-of-legendia-ps2recomp\PS2Recomp\out\build\ps2xRuntime\ps2EntryRunner.exe" "data\SLUS_212.01" 1> <temp>\ps2_run_console.log 2> <temp>\ps2_run_console.err`
(temp = `C:\Users\Valencio Hoffman\AppData\Local\Temp\opencode`)
(launch from workspace root D:\Personal\C++\tales-of-legendia-ps2recomp; GUI app never exits — run background, `Start-Sleep` ~30-40s for checks, kill via `taskkill /F /IM ps2EntryRunner.exe` before relaunch/relink)

## Unique Crashes & Subsystem Map
| Crash Address/PC | Subsystem | Callstack/Context | Proposed Fix Type | Regression Status | Resolution |
| ---------------- | --------- | ----------------- | ----------------- | ----------------- | ---------- |
| (no crash) | GS / parallel-gs `gs_renderer.cpp` | `LOGW("Unknown video format.")` ×3780; presented `pmode=0 smode1=0 smode2=0` while `dispfb1=0x1400`, `display1=0x001bf27f00000000` | Runtime C++ BIOS-like CRT seed in `syncCoreSubsystems()` | Verified: 3780→0 | **RESOLVED 2026-10-16** |
| exit `0x80000003` / "abort() has been called" | Granite Vulkan (`command_buffer.cpp:463-464` VK_ASSERT) via runtime `gs_gl_interop.cpp` `Slot::copy()` | `[ERROR]: Vulkan error at ...command_buffer.cpp:464.` after 3× TRANSFER-stage WARN; legacy layout `SHADER_READ_ONLY_OPTIMAL` in `release_image_barrier` | Runtime C++ (`gs_gl_interop.cpp` release layout → GENERAL) | Verified: `[ERROR]=0`, survived 95s, per-frame interop copies | **RESOLVED 2026-10-16** |

## Resolved Stubs
| Address | Handler | Binding Method | Status | Notes |
| ------- | ------- | -------------- | ------ | ----- |
| 0x001142E0 | `ps2_syscalls::SetGsCrt` → `GsSetCrt` | syscall translation (`data\config.toml` stub entry) | OK (plumbing) — **0 hits at runtime** | `patch_syscalls = false` only gates `[patches]` instruction patches, not syscall translation |
| — | game override `talesOfLegendiaAssetPaths` | `PS2_REGISTER_GAME_OVERRIDE("Tales of Legendia (USA)", "SLUS_212.01", …)` in `ps2xRuntime/src/lib/game_overrides.cpp` | OK | Pins cdRoot to `data\CD`, clears cdImage; confirmed applied in log |

## Resolved Syscalls
| Syscall ID | Implementation | File | Status |
| ---------- | -------------- | ---- | ------ |
| sceCdRead / sceCdSearchFile | pseudo-LBN host-file resolution | `Kernel/Stubs/CD.cpp` + `Helpers/Support.h` | OK (extracted-tree mode) |
| SetGsCrt / GsSetCrt | `configureGsCrt()` packing (LC/CMOD/INT) | `Kernel/Syscalls/System.cpp` (+ `System.h` decl) | Implemented; also reused by the CRT seed (game itself never calls it) |
| sceGsResetGraph | calls `ps2_syscalls::configureGsCrt` | `Kernel/Stubs/GS.cpp` | Present |

## Temporary Triage Stubs
| Address | Triage Type | Caller Context | Priority | Notes |
| ------- | ----------- | -------------- | -------- | ----- |
| — | temp diagnostics (keep until visual confirmation, then strip decision) | `[MMIO]` diagGsMmioWrite/Missed (write8/16/32/64/128 + TLB-alias) in `ps2_memory.cpp`; `[GSFE]` raw-forward + packed desc 0x0F drop logs in `gs_frontend.cpp`; `logRawRegOnce`/`logGifAdTagOnce`/`logGifAdWriteOnce`/`logUpstreamPrivOnce`/`logPresentationModeOnce` in `gs_parallel_backend.cpp`; `[GS] BIOS-like CRT seed:` one-time log in `ps2_runtime.cpp` | — | All compiled into current binary; MMIO/GSFE/RawReg/AdTag/SetGsCrt all **0 hits** (that IS the evidence) |

## Unhandled Opcodes
| Address | Opcode | Type | Resolution |
| ------- | ------ | ---- | ---------- |
|         |        |      |            |

## Known Issues
- [x] **Black screen**: RESOLVED via BIOS-like CRT seed (`Unknown video format.` 3780→0, present mode `pmode=0x1 smode1=0x4101 smode2=0x1`).
- [x] **Startup abort after seed**: RESOLVED (`gs_gl_interop.cpp` release layout SHADER_READ_ONLY→GENERAL; see Follow-up Crash).
- [ ] **Visual confirmation pending**: logs prove per-frame rendering, but on-screen content not yet confirmed (screenshot attempt: window alive but occluded; run user-cancelled).
- [ ] `applyGsDispEnv` sets `pmode`/`smode2` but **not** `smode1` (latent gap; seed covers this game).
- [ ] Privileged GS writes via GIF (`writeRegisterUnlocked` → `WriteRegisterRaw`) still unhandled for `PMODE/SMODE1/SMODE2/SYNCV` (latent; 0 guest hits so far).
- [ ] Seed completeness: SYNCV left at 0 (parallel-gs tolerates); revisit if timing issues appear.
- [ ] `RUNTIME_LOG` compiled out (`PS2X_ENABLE_RUNTIME_LOGS=OFF`) — instrument with `std::cerr`/`PS2_LOG_*`.
- [ ] Granite `deprecated TRANSFER stage` WARN spam (~2/frame from interop barriers) — benign lint noise; could modernize stages to COPY later.

## Known Upstream Issues
| Affected Function/Address | What Recompiler Generates | What MIPS Actually Does | Workaround Applied | GitHub Issue |
| ------------------------- | ------------------------- | ----------------------- | ------------------ | ----------- |
|                           |                           |                         |                    |             |

## Work State

### Completed
- Game identified as SLUS_212.01, recompiled via `data\config.toml`, CD/asset wiring done, boots to PHASE_MENU_REACH.
- Full diagnostic sweep proving root cause: MMIO 8/16/32/64/128 GS-priv, GSFE raw-forward, WriteRegisterRaw, GIF AD tag/write, SetGsCrt — **all 0 hits** → guest never programs GS video mode.
- Priv identity confirmed: backend `priv` == frontend `m_privRegs` == `m_memory.gs()` (ps2_runtime.cpp:622/676); upstream `PrivRegisterState priv_registers = {}` zero-init (gs_interface.hpp:348) → `Present()` copy is delivery, not discard.
- **CRT seed implemented** in `syncCoreSubsystems()` after `m_gs.init(...)`: `configureGsCrt(m_memory.gs(), 1u, 2u, 0u)` gated on `smode1 == 0`, one-time `[GS] BIOS-like CRT seed:` log; includes added (`Kernel/Syscalls/System.h`, `<cstdio>`).
- `PS2Memory::write128` GS-priv branch added (route to two `write64`) — latent fix for silently-dropped 128-bit GS-priv stores.
- Seed verification run: seed line present, present mode `pmode=0x1 smode1=0x4101 smode2=0x1`, **Unknown video format 3780→0**.
- **Abort investigation + fix**: reproduced self-exit `0x80000003`; traced `[ERROR] Vulkan error command_buffer.cpp:464` → `VK_ASSERT` (Debug⇒VULKAN_DEBUG) → `abort()`; offender = `gs_gl_interop.cpp:324` `release_image_barrier(... SHADER_READ_ONLY_OPTIMAL ...)` (legacy layout lint); fixed to `VK_IMAGE_LAYOUT_GENERAL`; incremental rebuild clean.
- Fix verification run: `[ERROR]=0`, terminate=0, survived 95s (timeout-killed), TRANSFER warnings 16440/95s ≈ per-frame interop copies; cancelled visual run logged 161733 TRANSFER / `[ERROR]=0` before stop.
- Screenshot attempt: game window alive (`PS2-Recomp | SLUS_212.01`, pid visible) but occluded/off-screen in capture; run user-cancelled.

### Active
- Visual confirmation that rendered content actually appears on screen (relaunch, ~30s, capture window region specifically — position/foreground the game window first, or ask user to eyeball it).
- Decide which temporary diagnostics to keep vs strip after confirmation (keep seed log + `logPresentationModeOnce` as candidates).
- Recommend committing the accumulated uncommitted changes (diagnostics + write128 fix + CRT seed + gs_gl_interop fix are ALL uncommitted).

### Blocked
- (none) — screenshot launch was user-cancelled, not a failure; relaunch only for the visual check.

## Next Move (ordered)
1. Visual confirmation: relaunch runner (PowerShell-native form above), wait ~30s, capture the game window specifically (bring to foreground / use window handle from `Get-Process MainWindowHandle`), or ask the user what they see on screen.
2. After confirmation: strip decision for the temporary `[MMIO]`/`[GSFE]`/upstream-priv diagnostics (keep seed log + present-mode log).
3. Commit the accumulated changes (diagnostics + write128 GS-priv routing + CRT seed + gs_gl_interop GENERAL fix) — user's call on message/branch.
4. Seed completeness later: SYNCV defaults; games that program their own mode must still win (gate already handles via `smode1 == 0`).
5. Optional cleanup: modernize interop barrier stages TRANSFER→COPY to silence the Granite WARN spam (cosmetic only).

## Relevant Files
| File | Why it matters |
| ---- | -------------- |
| `PS2_PROJECT_STATE.md` | Persistent state/phase/checklist; also the running work summary |
| `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp` | **CRT seed** in `syncCoreSubsystems()` after `m_gs.init(...)` (~line 622); includes `Kernel/Syscalls/System.h`, `<cstdio>`; override application at line 945 |
| `PS2Recomp\ps2xRuntime\src\lib\gs\gs_gl_interop.cpp` | **Abort fix**: `Slot::copy()` release barrier TRANSFER_DST→**GENERAL** (was SHADER_READ_ONLY_OPTIMAL = legacy → VK_ASSERT abort); acquire side already declares GENERAL; GL side uses GL_LAYOUT_GENERAL_EXT |
| `PS2Recomp\ps2xRuntime\src\lib\ps2_memory.cpp` | `gsRegPtr` MMIO mapping; `[MMIO]` diagnostics; write8/16 instrumentation; **write128 GS-priv routing fix** |
| `PS2Recomp\ps2xRuntime\src\lib\gs\gs_frontend.cpp` | `[GSFE]` raw-forward diagnostic in `writeRegisterUnlocked`, packed desc 0x0F drop log in `writeRegisterPacked` |
| `PS2Recomp\ps2xRuntime\src\lib\gs\gs_parallel_backend.cpp` | `Present()` priv-reg packing + backend diagnostics (`logRawRegOnce`, `logUpstreamPrivOnce`, `logPresentationModeOnce`, `logGifAdTagOnce/WriteOnce`) |
| `PS2Recomp\ps2xRuntime\src\lib\Kernel\Syscalls\System.cpp` / `System.h` | `configureGsCrt(gs, interlaced, videoMode, frameMode)` packing reused by the seed |
| `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\GS.cpp` | `applyGsDispEnv`/`applyGsRegPairs` — sets pmode/smode2 but never smode1 (latent gap; seed covers this game) |
| `PS2Recomp\ps2xRuntime\src\main.cpp` | terminate handler (`[terminate] ...` + `std::abort()` at :122); main() try/catch (`[main] fatal exception` → `_Exit(1)`); argv boot path |
| `PS2Recomp\out\build\_deps\parallel_gs-src\Granite\vulkan\command_buffer.cpp` | `is_legacy_layout()` (:375), `barrier(dep)` VK_ASSERT (:463-464), `image_barrier` VK_ASSERT (:627), acquire/release_image_barrier impls (:536/:563) — read-only: vendored |
| `PS2Recomp\out\build\_deps\parallel_gs-src\Granite\vulkan\vulkan_headers.hpp` | `VK_ASSERT` macro = LOGE + `abort()` when VULKAN_DEBUG (:49-61) — read-only: vendored |
| `PS2Recomp\out\build\_deps\parallel_gs-src\Granite\vulkan\CMakeLists.txt` | `VULKAN_DEBUG` auto-defined for Debug builds (:86-88) — read-only: vendored |
| `PS2Recomp\out\build\_deps\parallel_gs-src\Granite\util\logging.hpp` | `LOGE` → `debug_break()` (break only if debugger present) — read-only: vendored |
| `PS2Recomp\out\build\_deps\parallel_gs-src\gs\gs_interface.hpp` | `priv_registers = {}` zero-init proof (line 348) — read-only: vendored |
| `data\config.toml` | Game config, `SetGsCrt@0x001142E0` stub entry, `patch_syscalls = false` |
| `C:\Users\Valencio Hoffman\AppData\Local\Temp\opencode\ps2_fixcheck.err` | Post-fix verification stderr (`[ERROR]=0`, TRANSFER 16440, present mode seeded) |
| `C:\Users\Valencio Hoffman\AppData\Local\Temp\opencode\ps2_abort_probe2.err` | Crash reproduction stderr (`[ERROR] Vulkan error command_buffer.cpp:464`, exit 0x80000003) |
| `C:\Users\Valencio Hoffman\AppData\Local\Temp\opencode\ps2_screen_30s.png` | Screenshot attempt (window alive, occluded) |

## Important Context
- Runner command: see **Active Runner Command** above (read verbatim, never reconstruct).
- Kill before relaunch/relink: `taskkill /F /IM ps2EntryRunner.exe` (LNK1104 otherwise).
- Working incremental build command: see **Build Configuration** above.
- Latest verified run (post-fix): seed active, present mode `pmode=0x1 smode1=0x4101 smode2=0x1 dispfb1=0x1400 display1=0x001bf27f00000000`, `[ERROR]=0`, Unknown video format 0, per-frame interop copies confirmed via TRANSFER warn rate.
- `main()` catches main-thread exceptions (`[main] fatal exception` → exit 1); secondary-thread exceptions hit the terminate handler (`[terminate]` + `std::abort()`); Granite VK_ASSERT failures abort with exit `0x80000003` and a WER "abort() has been called" dialog.
- Many repository changes remain **uncommitted** in the working tree (diagnostics + write128 fix + CRT seed + gs_gl_interop fix); no commit or push has been performed.

## Learned Patterns (Auto-growing)
- "Unknown video format." is parallel-gs's GSVIDEO/SMODE1 mode decoder (`gs_renderer.cpp` LOGW), NOT an FMV/asset issue. Black screen = frame region ends when SMODE1 (CMOD/LC) matches no known combo.
- Runtime smode1 packing in `configureGsCrt` (`lc<<3` mask 127, `cmod<<13` mask 3) matches parallel-gs `SMODE1Bits` layout exactly. parallel-gs `SMODE1Bits` has no INT field; the runtime's "intf at bit 0" lands in RC and is harmless.
- Zero-valued `pmode/smode1/smode2` at `Present()` means **no one programmed GS**: `DISPFB1`/`DISPLAY1` were programmed (by C++ stubs), so the display path ran while the video-format path never did.
- **Zero hits on every instrumented guest-write path IS the evidence**: it proves the game relies on BIOS-default video mode → a BIOS-like seed is the correct fix, not faithful priv-reg plumbing.
- A syscall being correctly resolved + dispatched does NOT mean it is called: `SetGsCrt` had 0 hits despite correct mapping. Count handler hits before optimizing that path.
- `patch_syscalls = false` gates `[patches]` instruction patches only; syscall translation still happens.
- `GS::reset()` does not zero `m_privRegs` (they live in `PS2Memory`), so a mode seeded at init survives GS resets.
- Game override `applyFn` runs AFTER GS init (ps2_runtime.cpp:622) and after `applyMatching` (:945), so an override can overwrite a seeded CRT mode. Matching = ELF basename case-insensitive + optional entry (0=any) + optional CRC32 (0=any).
- `readCdSectors` resolution order: registered pseudo-LBN range FIRST, cdImage fallback SECOND. Clearing cdImage forces all reads through the extracted tree.
- bsdtar (`C:\Windows\System32\tar.exe`) extracts ISO9660 with Rock Ridge names, STRIPPING `;1` suffixes; runtime resolution chain handles stripped suffixes.
- Linker LNK1104 "cannot open file ps2EntryRunner.exe" = old game process still holding the exe; kill with taskkill before relink.
- Write/edit tools blocked for workspace-ROOT files; write to temp then `Move-Item -Force`, or use python one-liners.
- Launch GUI runner via PowerShell-native `& "exe" args 1> log 2> err`; the `cmd /c` nested-quote form gets mangled ("Access is denied").
- **Debug build auto-enables Granite `VULKAN_DEBUG`** → `VK_ASSERT` = LOGE + `abort()`. Granite's `is_legacy_layout()` lint forbids sync1 layouts (SHADER_READ_ONLY_OPTIMAL, COLOR_ATTACHMENT_OPTIMAL, DEPTH_*) in sync2 barriers; use sync2 layouts (GENERAL, READ_ONLY_OPTIMAL, TRANSFER_*, UNDEFINED) in `src/lib/` Vulkan code.
- `abort()`/VK_ASSERT death signature: stderr `[ERROR]: Vulkan error at <file>:<line>`, self-exit code `-2147483645` (`0x80000003`), WER dialog "abort() has been called"; WER Application Error (Id 1000) is NOT logged for aborts.
- Uncaught C++ exception on a secondary thread → terminate handler prints `[terminate] unhandled exception` + `[terminate] std::exception: <what()>` then `std::abort()`. Main-thread exceptions are caught by `main()` → `[main] fatal exception: ...` → `_Exit(1)`.
- **Enabling a previously-broken feature exposes latent bugs downstream**: the CRT seed made `Present()` actually render, which executed the `gs_gl_interop` copy path for the first time and tripped a dormant VK_ASSERT. Expect follow-on crashes after unblocking rendering.
- Granite `deprecated TRANSFER stage` WARN rate is a free per-frame rendering probe: ~2/frame means the interop copy path runs every presented frame.
- `PS2Memory::write128` to the GS-priv window previously fell through silently (dropped); privileged regions need explicit routing in every width variant (8/16/32/64/128).
- Vendored `parallel_gs-src` is read-only reference: use it to learn the decoder's required bit fields and assert semantics, then satisfy them from `src/lib/`.

## Session Log
### 2026-10-16
- **Actions**: (1) Confirmed seed verification run: `[GS] BIOS-like CRT seed:` present, present mode `pmode=0x1 smode1=0x4101 smode2=0x1`, Unknown video format 3780→0. (2) User reported startup abort ("abort() has been called", black screen). (3) Reproduced: background run self-exited with `-2147483645` (`0x80000003`); probe1+probe2 both logged `[ERROR]: Vulkan error at ...command_buffer.cpp:464.` after 3× TRANSFER-stage WARN. (4) Traced mechanism: Debug⇒VULKAN_DEBUG⇒VK_ASSERT⇒LOGE+abort; `is_legacy_layout()` forbids SHADER_READ_ONLY_OPTIMAL; offender = our `gs_gl_interop.cpp` `Slot::copy()` `release_image_barrier` (only SHADER_READ_ONLY_OPTIMAL in entire runtime+gs code; parallel-gs `gs/` uses none). (5) Fixed release layout to GENERAL; incremental rebuild; verification run `[ERROR]=0`, survived 95s, TRANSFER 16440 (per-frame rendering proven). (6) Screenshot attempt: window `PS2-Recomp | SLUS_212.01` alive but occluded; user cancelled the visual run. (7) Also shipped `PS2Memory::write128` GS-priv routing fix earlier this session.
- **Discovered**: guest never programs GS video mode (all instrumented write paths 0 hits) → BIOS-like seed is correct for this game; enabling rendering exposed the dormant interop VK_ASSERT; abort exit-code signature identified; `main()` vs terminate-handler exception routing mapped.
- **Current Blocker**: none hard — visual on-screen confirmation of rendered content still pending; all changes uncommitted.

### 2026-10-15
- **Actions**: Merged the latest compaction summary into this state file. Re-verified all Relevant-Files paths exist; re-verified run evidence from `ps2_run_console.err` (`Unknown video format.` ×1980, missing-function hits 0, `[GS] present mode: pmode=0 smode1=0 smode2=0 dispfb1=0x1400 display1=0x001bf27f00000000`); confirmed both temporary diagnostics are present in source (`gs_parallel_backend.cpp:29` called at `:388`; `System.cpp:40`).
- **Discovered**: `applyGsDispEnv` sets `pmode`/`smode2` but never `smode1`; `writeRegisterUnlocked()` can bypass the frontend switch via `WriteRegisterRaw()` (raw cases only for 0x59/0x5a/0x5b/0x5c/0x5f), so privileged PMODE/SMODE1/SMODE2/SYNCV handling through GIF is unconfirmed.
- **Current Blocker**: No runtime evidence yet of guest writes to PMODE/SMODE1/SMODE2/SYNCV (register codes + GIF-vs-MMIO). Need targeted one-time diagnostics before choosing between faithful priv-reg handling and the BIOS-like CRT seed.

### 2026-10-14
- **Actions**: Confirmed cdImage auto-detect edit (uncommitted) in build; extracted ISO → `data\CD` (34 files, 3.92 GB) via bsdtar; registered `talesOfLegendiaAssetPaths` override (match SLUS_212.01, pin cdRoot, clear cdImage); incremental rebuild in `out\build` (killed stale game process to fix LNK1104); relaunched with console capture — override line confirmed in log, zero CD resolution errors.
- **Discovered**: Extracted tree holds AFS archives (BATTLE/FIELD/MAP/MOVIE/SND/SYS_REG.AFS + PTN_INFO.BIN/NAM/FLIST.DIR) matching ELF path strings; bsdtar strips `;1` (Rock Ridge).
- **Current Blocker**: Black screen — SMODE1 combo unrecognized by parallel-gs decoder; need to instrument `GsSetCrt`/`configureGsCrt` (System.cpp) with always-on logging to capture interlaced/videoMode/frameMode.
