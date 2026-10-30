# 16 — ps2_recomp (TOML → C++) + TOML Reference

> **Source:** PS2Recomp wiki `Ps2xRecomp.md`.
> **Binary:** `ps2_recomp` (target in `ps2xRecomp/`).
> **Use when:** tuning `stubs`/`skip`/`patches`, or deciding between a stub, a skip, a syscall implementation, and an override.

---

## What the recompiler does

`ps2_recomp` consumes the TOML from `ps2_analyzer` and translates PS2 **MIPS** into **C++**. Each MIPS instruction becomes a C++ operation against an emulated CPU context.

```
addiu $r4, $r4, 0x20        →        ctx->r4 = ADD32(ctx->r4, 0x20);
```

That is the whole idea: **registers are fields on a context**, and MIPS semantics are rewritten into C++ operations.

> ⚠️ **The output is machine code, not source you own.** Never hand-edit `runner/*.cpp`. See §Absolute Prohibitions in `SKILL.md`. Fixes belong in TOML, runtime C++, or game overrides.

---

## How the recompiler interprets each TOML directive

### `stubs`

Functions in `stubs` are **not recompiled**. The recompiler emits small wrapper functions that call known runtime syscall/stub handlers **by name**.

Two forms:

```toml
stubs = [
  "sceCdRead",                # name only — resolve by known symbol
  "sceCdRead@0x00123456",     # name@address — bind a stripped address to a handler
]
```

This is the primary tool for **stripped games** — you supply the missing name→address mapping.

### `skip`

Functions in `skip` are **not recompiled** and are treated as "intentionally unsupported". The recompiler emits an explicit placeholder:

```cpp
ps2_stubs::TODO_NAMED("FunctionName")
```

It makes outstanding manual work **visible in the generated code** instead of silently absent.

### Choosing between them — decision table

| Situation | Use | Effect |
|-----------|-----|--------|
| You know the address and want it routed to an existing runtime handler | **`stubs`** (`"handler@0xADDR"`) | Generated wrapper calls that handler |
| You want an explicit TODO marker for now | **`skip`** | `ps2_stubs::TODO_NAMED(...)`, returns an error-like value |
| Runtime logs show unknown **numeric** syscalls (`[Syscall TODO] ...`) | **implement `ps2xRuntime` syscalls** | Behaviour keyed by syscall ID, not address |
| You need custom logic, or per-build routing without touching generated code | **Game override** | See `18-game-override-hooks.md` |

---

## TOML Reference (verified against `config_manager.cpp`)

The analyzer emits these; the recompiler reads them.

### `[general]`

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `input` | string | required | Path to the source ELF |
| `output` | string | required | Output folder for generated C++ |
| `ghidra_output` | string | `""` | Optional Ghidra function-map CSV |
| `single_file_output` | bool | `false` | `true` = one combined `.cpp`; `false` = one file per function |
| `patch_syscalls` | bool | `false` | Apply configured patches to `SYSCALL` instructions — **`false` recommended** |
| `patch_cop0` | bool | `false` | Apply configured patches to COP0 instructions |
| `patch_cache` | bool | `false` | Apply configured patches to `CACHE` instructions |
| `stubs` | array | `[]` | Names/`name@addr` forced to stubs (overrides detection) |
| `skip` | array | `[]` | Names forced to skipped wrappers (overrides detection) |

> **Both placements work.** `stubs` and `skip` are read from `[general]` **or** from the TOML root — the parser falls back to top level if `[general]` lacks the array. Pick one and stay consistent.

### `[patches]`

| Key | Type | Meaning |
|-----|------|---------|
| `instructions` | array | Raw instruction replacements by address (manual patch list) |

---

## Running it

```bash
./ps2_recomp config.toml
```

Reads the TOML, loads the ELF, writes C++ into `general.output`.

Built binary path (this repo, Debug/MSVC layout):

```
PS2Recomp/out/build/ps2xRecomp/Debug/ps2_recomp.exe
```

---

## After recompilation — mandatory review

**The tool is not perfect.** After generating, you must scan the output for untranslatable instructions. They are marked with comments:

```cpp
// Unhandled opcode: <raw>
// Unhandled FPU.S instruction: <raw>
// Unhandled PMTHL instruction: <raw>
```

If your game uses an instruction (or a special-case behaviour) the recompiler does not support yet, these markers are where it surfaces.

```powershell
# Find every untranslated marker in the generated output
rg -n "Unhandled opcode|Unhandled FPU\.S|Unhandled PMTHL" <general.output>
```

### Manual verification checklist

1. Functions that **should** be stubs are actually stubbed.
2. Functions that **should** be skipped are actually skipped.
3. Required **patches** are correct — and nothing important was patched away.
4. Every `// Unhandled ...` marker is either benign or understood.

Only when the generated code builds cleanly *and* `stubs`/`skip`/`patches` are right should you move to the runtime stage.

---

## How to infer what an unknown `sub_xxx` should do

You cannot infer semantics from an address alone. Combine static and dynamic evidence.

### Static clues

| Clue | What it tells you |
|------|-------------------|
| **Caller context** | Inspect *all* callsites of the address |
| **Arguments** | What is set up in `$a0..$a3` |
| **Return contract** | How the caller tests `$v0` — zero/non-zero, pointer, negative error |
| **Side effects** | Writes to known structs, DMA/GS/VIF regions, file paths, RPC payloads |
| **Nearby strings/constants** | `cdrom0:` paths, format strings, module names |

### Dynamic clues

| Clue | Source |
|------|--------|
| Stub fallback logs | `name`, `PC`, `RA`, and arguments |
| Syscall TODO logs | Encoded syscall ID + register state |
| Runtime logs | Repeated PC (spin), unresolved function addresses, thread state |

### Practical strategy

1. **First** try existing runtime handlers (`sceCdRead`, `fio*`, `Sif*`, …).
2. **If uncertain**, use `ret0` / `ret1` / `reta0` to test whether the call is just a probe.
3. **If the game progresses**, replace temporary return stubs with real behaviour later.
4. **If the game breaks/hangs**, implement real semantics — or a targeted override.

Full triage workflow: `19-stripped-game-walkthrough.md`. Deeper reasoning: `13-decisional-brain.md`.

---

## Cross-references

- Analyzer / function discovery → `15-ps2x-analyzer.md`
- Runtime behaviour per directive → `17-ps2x-runtime.md`
- Override API → `18-game-override-hooks.md`
- Working TOML example → `examples/toml-config-template.toml`
- MIPS→C++ translation details → `02-mips-r5900-isa.md`
