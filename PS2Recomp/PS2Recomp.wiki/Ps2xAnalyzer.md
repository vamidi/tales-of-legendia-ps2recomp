# PS2 ELF Analyzer Tool

The PS2 ELF Analyzer generates a **TOML configuration file** that will later be consumed by the recompiler to produce **C/C++ recompiled code**.

At a high level, the analyzer tries to discover:
- functions (names + start/end addresses),
- library calls that should become stubs,
- functions to skip (startup/runtime glue),
- and CPU instructions that require patching.

---

## How the Analyzer finds code

The analyzer supports three distinct paths for discovering functions inside a PS2 ELF.

### 1) DWARF debug information (best case)
If the ELF was built with debug symbols (e.g. `-g`), the analyzer uses **libdwarf** to extract:
- correct function names,
- exact start/end addresses.

This is common in homebrew builds, prototypes, or development builds.

### 2) Native heuristic scanner (retail / stripped ELFs)
Most commercial games are stripped, meaning there are no symbols.

For these cases, the analyzer uses a native heuristic approach (the **JAL Scanner**) to detect function boundaries by scanning for common MIPS calling patterns (like `jal` calls and prologue/epilogue patterns).

> This method is fast and works well on many retail games, but it may produce incomplete or imperfect boundaries depending on compiler settings and anti-reversing techniques.

### 3) Ghidra integration (complex / protected games)
Some games use patterns that confuse scanners (heavy inlining, custom calling conventions, packed code, anti-debug tricks, etc.).  
For those cases, the analyzer can consume a function map exported from **Ghidra** to improve accuracy.

---

## Using the Analyzer

### Arguments:

* `input_elf`: Path to the PS2 ELF file.
* `output_toml`: Path where the generated TOML configuration will be saved.

### Command line

```bash
ps2_analyzer <input_elf> <output_toml>
```

### Example:
```bash
ps2_analyzer SLUS_203.12 game_config.toml
```

## Generated TOML layout

The tool outputs a TOML file with sections like:

- `[general]`
  - paths (input ELF, optional ghidra map, etc.)
- `stubs`
  - list of imported/library functions that should be replaced by C/C++ stubs
- `skip`
  - list of functions that should be ignored (startup code, entry/initialization routines, etc.)
- `[patches]`
  - individual instruction patches (examples: `syscall`, privileged COP0 ops, or other sequences that must be replaced/emulated)

> Exact keys may evolve as the project grows, but these are the main categories the recompiler expects.

---

## Special case: using Ghidra for protected or tricky games

Some games include strong anti-reversing measures or simply produce code patterns that are hard to infer reliably with heuristics.  
In these cases, you can use Ghidra to export a function map and feed it into the analyzer output TOML.

### Step 1: Install the PS2 Ghidra addon

Install **ghidra-emotionengine-reloaded**:
- https://github.com/chaoticgd/ghidra-emotionengine-reloaded

### Step 2: Export a function map from Ghidra

1. Open the PS2 ELF in Ghidra (using the Emotion Engine loader).
2. Run the provided export script:
   - `ps2xRecomp/tools/ghidra/ExportPS2Functions.py`
   - or the equivalent `.java` version, if your setup uses Java scripts
3. The script exports a **CSV** file containing the discovered functions.

### Step 3: Reference the exported CSV in your TOML

After generating the TOML, add (or edit) the Ghidra map path under `[general]`, for example:

```toml
[general]
ghidra_output = "path/to/map.csv"
```
 