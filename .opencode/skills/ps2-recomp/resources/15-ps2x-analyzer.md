# 15 — ps2_analyzer (ELF → TOML)

> **Source:** PS2Recomp wiki `Ps2xAnalyzer.md`.
> **Binary:** `ps2_analyzer` (target in `ps2xAnalyzer/`).
> **Use when:** generating a config for the first time, or when function boundaries look wrong on a stripped ELF.

---

## What it produces

`ps2_analyzer` reads a PS2 ELF and writes a **TOML configuration** consumed by `ps2_recomp`. It tries to discover:

- **functions** — names + start/end addresses
- **library calls** that should become **stubs**
- **functions to skip** (startup/runtime glue that must not be recompiled)
- **CPU instructions that require patching** (`syscall`, privileged COP0 ops, etc.)

It does **not** emit code. It emits intent.

---

## The three discovery paths

The analyzer finds code through three different mechanisms, in order of preference.

### 1 — DWARF debug information (best case)

If the ELF was built with debug info (`-g`), the analyzer uses **libdwarf** to extract:

- correct function names
- exact start/end addresses

Common on homebrew builds, prototypes, and internal development builds. If you have this, you have already won.

### 2 — Native heuristic scanner (retail / stripped)

Most commercial games are **stripped** — no symbols at all. The analyzer falls back to a native heuristic approach, the **JAL Scanner**, which detects function boundaries by scanning for common MIPS calling patterns (`jal` targets and prologue/epilogue idioms).

> Fast and effective on many retail titles, but it can produce **incomplete or imperfect boundaries** depending on compiler settings, heavy inlining, and anti-reversing tricks.

**Symptom of bad boundaries:** `Function at address 0x... not found` at runtime, or generated functions that "fall through" into the next function's body. Fix with path 3.

### 3 — Ghidra function map (complex / protected games)

For games that defeat the scanner — heavy inlining, custom calling conventions, packed/encrypted code, anti-debug — export a function map from **Ghidra** and feed it to the analyzer.

---

## Usage

### Arguments

| Arg | Meaning |
|-----|---------|
| `input_elf` | Path to the PS2 ELF |
| `output_toml` | Where the generated TOML is written |

```bash
ps2_analyzer <input_elf> <output_toml>
```

### Example

```bash
ps2_analyzer SLUS_203.12 game_config.toml
```

Built binary path (this repo, Debug/MSVC layout):

```
PS2Recomp/out/build/ps2xAnalyzer/Debug/ps2_analyzer.exe
```

### Generated TOML shape

```toml
[general]
input  = "path/to/game.elf"
output = "path/to/output"
# ... plus patch flags

stubs = [ ... ]   # or [general].stubs — both are accepted
skip  = [ ... ]   # or [general].skip

[patches]
instructions = [ ... ]
```

| Section | Contents |
|---------|----------|
| `[general]` | Paths (input ELF, optional Ghidra map, output dir) and behaviour flags |
| `stubs` | Imported/library functions to replace with C/C++ stubs |
| `skip` | Functions to ignore (startup code, entry/init routines) |
| `[patches]` | Individual instruction patches (`syscall`, privileged COP0 ops, sequences that must be replaced) |

> Exact keys evolve with the project. For the **authoritative** key list as implemented here, see `16-ps2x-recomp.md` §TOML Reference — it is verified against `ps2xRecomp/src/lib/config_manager.cpp`.

---

## Wiring in a Ghidra function map

### Step 1 — install the PS2 Ghidra addon

**ghidra-emotionengine-reloaded**:
<https://github.com/chaoticgd/ghidra-emotionengine-reloaded>

### Step 2 — export the function map

1. Open the PS2 ELF in Ghidra using the **Emotion Engine** loader.
2. Run the export script:
   - `ps2xRecomp/tools/ghidra/ExportPS2Functions.py`
   - or the equivalent `.java` version if your setup runs Java scripts.
3. The script produces a **CSV** of discovered functions.

### Step 3 — point the TOML at it

```toml
[general]
ghidra_output = "path/to/map.csv"
```

Then re-run `ps2_recomp` against the updated TOML.

---

## When to reach for the Ghidra path

| Signal | Action |
|--------|--------|
| Many `Function at address 0x... not found` at runtime | Export map, re-analyze |
| Generated function bodies clearly overrun into neighbors | Export map, re-analyze |
| Game is known-protected / packed | Export map **before** first recompile |
| Analyzer output looks plausible and boots | Stay on heuristics |

Skipping the Ghidra step on a hard title costs more time than doing it up front — bad boundaries produce fake crashes that look like runtime bugs.

---

## Cross-references

- TOML semantics and patch flags → `16-ps2x-recomp.md`
- Ghidra scripting + MCP → `05-ghidra-ghydramcp-guide.md`
- `sub_xxx` inference → `06-game-porting-playbook.md`, `19-stripped-game-walkthrough.md`
- ELF input validation → `14-ps2-elf-files.md`
