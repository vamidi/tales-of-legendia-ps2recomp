# 14 — PS2 ELF Files (Input Format)

> **Source:** PS2Recomp wiki `PS2-ELF-Files.md`.
> **Use when:** locating the game executable inside an ISO, verifying you have the right binary, or explaining why a file has no extension yet is still an ELF.

---

## What an ELF is

**ELF** = **E**xecutable and **L**inkable **F**ormat. It is the standard container for executables, object files, and shared libraries on the PS2 as well.

On PS2, the game's main executable **is an ELF file** — even when it does not carry a `.elf` extension. The boot ELF is the only binary `ps2_analyzer` will accept as `input`.

---

## What's inside a PS2 ELF

The format itself is well documented and standard tooling works on PS2 executables. For recompilation work, the **section layout** is what matters.

| Section | Contents | Why you care |
|---------|----------|--------------|
| `.text` | Executable code (functions) | The only thing that gets recompiled |
| `.data` | Initialized globals/statics | Guest data you see in `rdram` at load |
| `.bss` | Uninitialized globals/statics (size only in file) | Allocated at runtime; zero-filled |
| `.rodata` | Read-only data — constants, strings, jump tables | Const tables are prime `sub_xxx` inference clues |
| `.sdata` | Small initialized data (MIPS small-data area) | Reached via `$gp`-relative addressing |
| `.sbss` | Small uninitialized data | Same, uninitialized |
| `.ctors` | C++ constructors | Run before `main`; often a good `skip` candidate list |
| `.dtors` | C++ destructors | Usually irrelevant to porting progress |
| `.eh_frame` | Exception/unwind metadata (DWARF2) | Ignore for recompilation |
| `.jcr` | GCJ Java-related | Often present even with no Java; ignore |
| `.mdebug` | Extended debug/symbol info for some GDB versions | A symbol source when DWARF is absent |
| `abs` | Absolute symbols | Symbol source only |
| `extern` | External symbols | Imports → stub candidates |

> **Retail reality:** not every section is present. Commercial builds are usually **stripped**, so `.mdebug`/`.eh_frame` debug info may be gone entirely. That is exactly the case the JAL heuristic scanner (see `15-ps2x-analyzer.md`) exists for.

Reference overview: <https://www.retroreversing.com/ps2-unstripped/>

---

## Extracting the executable from a disc image

After dumping a **legally owned** disc to ISO, you will often **not** find a file literally named `something.elf`. That is normal — PS2 executables usually have no extension.

### Naming conventions

| Pattern | Region / publisher |
|---------|-------------------|
| `SLUS_XXXXX` / `SLUS-XXXXX` | North America |
| `SLES_XXXXX` / `SLES-XXXXX` | Europe |
| `SLPS_XXXXX` / `SLPS-XXXXX` | Japan |
| `SCUS_XXXXX`, `SCES_XXXXX`, … | Sony / first-party |

These live in the **root of the ISO filesystem**.

### Extraction

Any ISO tool that handles PS2 images works — **7-Zip** generally browses and extracts them fine.

### Verify it is actually an ELF

Check the magic bytes — an ELF starts with:

```
0x7F 0x45 0x4C 0x46     # "\x7FELF"
```

```powershell
# First 4 bytes as hex
Format-Hex -Path .\SLES_531.55 -Count 4
```

`7F 45 4C 46` → proceed. Anything else → you grabbed the wrong file (a `SYSTEM.CNF`, an IRX, or a container like `COREC.BIN`).

---

## How this maps onto the pipeline

| Artifact | Extension | Format | Handled by |
|----------|-----------|--------|-----------|
| Main executable `SLES_531.55` | **none** | ELF | `ps2_analyzer` → `ps2_recomp` |
| Secondary `icon.elf` | `.elf` | ELF | Same pipeline, separate TOML |
| `COREC.BIN` / `.img` | `.bin`, `.img` | **Not ELF** | Runtime only — must be loaded by the game |
| IOP modules `*.IRX` | `.irx` | IRX | Runtime (`ps2xIOP`), never recompiled |

See `db-overlay-patterns.md` for multi-binary / overlay games, and `05-ghidra-ghydramcp-guide.md` for loading the ELF into Ghidra.

---

## Hard rules

1. **Verify magic bytes before analyzing.** A mis-identified input produces an empty or nonsense TOML and hours of confusion.
2. **One TOML per ELF.** Region and revision change the address map; never reuse a TOML across ELFs (see `19-stripped-game-walkthrough.md` §10).
3. **`SYSTEM.CNF` is not the executable** — it is the boot descriptor pointing at it (`BOOT2 = cdrom0:\SLES_531.55;1`). Use it to *find* the ELF.
