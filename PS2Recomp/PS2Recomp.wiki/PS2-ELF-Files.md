# PS2 ELF Files (Executable and Linkable Format)

## What is an ELF file?

**ELF** stands for **Executable and Linkable Format**. It’s a widely used standard format for executable binaries (programs), object files, and shared libraries.

On the **PlayStation 2**, games use ELF binaries as their main executable format. In practice, the “game executable” you boot is an ELF file (even if it doesn’t always have a `.elf` extension).

---

## What’s inside a PS2 ELF?

The ELF format is well documented online, and most tools that understand ELF will work with PS2 executables too.  
For PS2 reverse engineering and recompilation work, the most useful thing to understand is the **section layout**.

Reference (good overview):  
- https://www.retroreversing.com/ps2-unstripped/

### Common sections

The ELF format contains many sections, and most of them are common across platforms:

| Section    | Description |
|-----------|-------------|
| `.text`   | Contains the executable code (functions). |
| `.data`   | Initialized global/static variables. |
| `.bss`    | Uninitialized global/static variables (allocated at runtime, but stored as size-only in the ELF). |
| `.rodata` | Read-only data (constants, strings, constant tables). |
| `.sdata`  | Small initialized data (common on MIPS). |
| `.sbss`   | Small uninitialized data (common on MIPS). |
| `.ctors`  | C++ constructors. |
| `.dtors`  | C++ destructors. |
| `.eh_frame` | Exception/unwind metadata (DWARF2). See: https://www.airs.com/blog/archives/460 and https://gcc.gnu.org/wiki/Dwarf2EHNewbiesHowto |
| `.jcr`    | GCJ Java-related section (often present even if Java is not used). |
| `.mdebug` | Extended debug/symbol info used by some GDB versions. |
| `abs`     | Absolute symbols. |
| `extern`  | External symbols. |

> Note: Not every PS2 ELF will contain all sections. Retail builds are often stripped, so debug-related sections may be missing.

---

## How to extract the PS2 ELF from a game

After dumping your **legally owned** PS2 disc to an ISO, you might not immediately see a file named `something.elf`.  
That’s normal: many PS2 executables **do not use the `.elf` extension**, but they still *are* ELF binaries.

### Typical PS2 executable naming convention

The main executable is commonly named like:

- `SLUS_XXXXX` / `SLUS-XXXXX` (North America)
- `SLES_XXXXX` / `SLES-XXXXX` (Europe)
- `SLPS_XXXXX` / `SLPS-XXXXX` (Japan)
- `SCUS_XXXXX`, `SCES_XXXXX`, etc. (first-party / Sony-published titles)

These files are usually located in the **root of the ISO filesystem**.

### Quick extraction options

You can extract the executable using any ISO tool that supports PS2 images, for example:

- **7-Zip** (often works for browsing/extracting files)

Once extracted, you can verify the file is an ELF by checking the magic bytes:

- ELF files start with: `0x7F 45 4C 46` (which reads as `\x7FELF`)

---
