<!-- ⛔ RULES — Re-read this section EVERY TIME you open this file. -->
<!-- These rules also appear in the SKILL.md. Redundancy is intentional. -->

# ⛔ QUICK RULES (mandatory re-read)

1. **Build:** `cmake --build <build_dir>` — NEVER add `--clean-first`, `--target clean`, or delete the build directory.
2. **Files:** NEVER modify `runner/*.cpp` or generated `data/output/*.cpp`. Fix in `src/lib/` or game overrides.
3. **Headers:** NEVER modify `.h` without asking user. Triggers mass rebuild.
4. **Git:** NEVER use destructive git commands (`checkout`, `clean`, `reset`, `stash`, `pull`).
5. **Verify:** NEVER assume file names/paths. Use tools (`list_dir`, `find_by_name`, `grep_search`).
6. **Skill location:** `.opencode/skills/ps2-recomp/` (resources in `resources/`).

---

# PS2 Recomp — Project State
> Auto-maintained by agent. DO NOT DELETE. Read at session start, update after every major action.

## Boot Status
- [x] Loaded `03-ps2recomp-pipeline.md`
- [x] Loaded `04-runtime-syscalls-stubs.md`
- [x] Verified comprehension (3 questions answered)

## Game Info
- **Title**: Tales of Legendia
- **Region**: NTSC-U (SLUS-21201, disc VER 1.01)
- **Has Symbols**: no (stripped) — function boundaries from Ghidra export `map.csv`

## Workspace Paths
- **PS2Recomp Repo**: `D:\Personal\C++\tales-of-legendia-ps2recomp\PS2Recomp`
- **Game Workspace**: `D:\Personal\C++\tales-of-legendia-ps2recomp\data`
- **ISO Path**: `D:\Personal\C++\tales-of-legendia-ps2recomp\data\Tales of Legendia (USA).iso`
- **Output Dir**: `D:\Personal\C++\tales-of-legendia-ps2recomp\data\output` (per `config.toml`; not yet generated)

## Binaries
| Role | File Name | Type | Path | TOML Config | Status |
|------|-----------|------|------|-------------|--------|
| Main | SLUS_212.01 | ELF | `D:\Personal\C++\tales-of-legendia-ps2recomp\data\SLUS_212.01` | `data\config.toml` | analyzed / not recompiled |

## Environment Setup
- **Ghidra Install Path**: unknown
- **Tools**: `PS2Recomp\out\build\ps2xRecomp\Debug\ps2_recomp.exe` (copy at `data\ps2_recomp.exe`, identical, built 2026-10-10 17:54 — includes uncommitted edits to config_manager.cpp / ps2_recompiler.cpp / r5900_decoder.cpp)

## Current Phase
PHASE_RUNTIME_BUILD

## Build Configuration
- **CMake Generator**: Visual Studio 18 2026 (`PS2Recomp\out\build`) — ❌ slow for runner build; switch to Ninja + clang-cl + Release suggested, awaiting user decision
- **C++ Compiler**: MSVC cl.exe 19.51 (toolset 14.51.36231)
- **Build Type**: Debug (multi-config)
- **Ghidra CSV Path**: `D:\Personal\C++\tales-of-legendia-ps2recomp\data\map.csv`
- **single_file_output**: false

## PCSX2 MCP Status
- **Status**: Not Connected
- **Game Loaded**: N/A
- **Match**: N/A

## Active Runner Command
<!-- ACTIVE RUNNER COMMAND: (not yet — runner exe not built) -->

## Unique Crashes & Subsystem Map
| Crash Address/PC | Subsystem | Callstack/Context | Proposed Fix Type | Regression Status | Resolution |
| ---------------- | --------- | ----------------- | ----------------- | ----------------- | ---------- |
|                  |           |                   |                   |                   |            |

## Resolved Stubs
| Address | Handler | Binding Method | Status | Notes |
| ------- | ------- | -------------- | ------ | ----- |
| 0x001142E0–0x00114B50 | 55 kernel/SIF syscall wrappers | `config.toml` stubs | configured | Auto-classified by ExportPS2Functions |

## Resolved Syscalls
| Syscall ID | Implementation | File | Status |
| ---------- | -------------- | ---- | ------ |
|            |                |      |        |

## Temporary Triage Stubs
| Address | Triage Type | Caller Context | Priority | Notes |
| ------- | ----------- | -------------- | -------- | ----- |
|         |             |                |          |       |

## Unhandled Opcodes
Scan of `data/output` (2026-10-10): 5,314 `Unhandled ...` throws, ALL in 2 files — both are data misidentified as code by Ghidra.
| Address | Opcode | Type | Resolution |
| ------- | ------ | ---- | ---------- |
| 0x3A3198–0x3C74E0 (`entry_003a3198`, 3,608 hits) | various (raw words 0x4,0x8,0xC…) | DATA (int tables) | ✅ TOML `skip` (2026-10-10) → 0 unhandled |
| 0x3C74E0–0x3C7590 (`entry_003c74e0`, 0 hits) | — | DATA (doubles, 0x3FF00000=1.0) | ✅ TOML `skip` (2026-10-10) → 0 unhandled |
| 0x3C7590–0x3D1480 (`entry_003c7590`, 1,706 hits) | various (ASCII "IRC\n", "CCG ") | DATA (strings) | ✅ TOML `skip` (2026-10-10) → 0 unhandled |

## Known Issues
- [x] DECISION: User chose to KEEP VS 2026 + MSVC + Debug build dir (`PS2Recomp\out\build`). Do not suggest switching again.
- [x] Workflow (user decision B2): recompile to `data\output`, then `cp -f data/output/* PS2Recomp/ps2xRuntime/src/runner/` after EVERY recompile. Done 2026-10-10 19:30 (53,222 cpp + 2 h). ps2EntryRunner.exe (19:07) is STALE until rebuilt.
- [ ] ELF is one merged rwx LOAD segment (0x100000–0x3DAF80 file, memsz to 0x4E9B80), no named sections → code/data boundary unknown to analyzer.
- [ ] Unmapped gap 0x388BB8–0x3A3198 in map.csv; contains kernel-mode code at ~0x388BCC (`lui k0,0x8007`) — likely exception/handler stub copied to kernel RAM. Low priority.

## Known Upstream Issues
| Affected Function/Address | What Recompiler Generates | What MIPS Actually Does | Workaround Applied | GitHub Issue |
| ------------------------- | ------------------------- | ----------------------- | ------------------ | ----------- |
|                           |                           |                         |                    |             |

## Learned Patterns (Auto-growing)
- Huge Ghidra "functions" (100KB+) whose unhandled raw words look like ASCII/small ints are data regions; fix with TOML `skip`, not C++.
- `ps2EntryRunner` only compiles `ps2xRuntime/src/runner/*.cpp`; recompiler output must land there (or be copied) before building.

## Session Log
### 2026-10-10
- **Actions**: Boot sequence; workspace inspection; created this state file.
- **Discovered**: ELF + Ghidra map + config.toml (55 stubs, 11549 uncategorized) present; recompiler never run; runtime lib built (Debug) but no game runner.
- **Current Blocker**: ps2EntryRunner must be rebuilt with real runner sources (awaiting user OK; multi-hour MSVC Debug build).
- **Recompile 2**: added 3 data-region skips to config.toml → 0 unhandled opcodes; output copied to `src\runner`.
