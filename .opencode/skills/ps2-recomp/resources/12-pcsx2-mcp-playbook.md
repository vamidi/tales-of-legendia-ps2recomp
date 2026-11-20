# PCSX2 MCP Playbook — Runtime Debugging & A/B Comparison

> **Purpose:** Use PCSX2 running the *original* PS2 game as ground truth. Compare its register/memory state against the recompiled output to find *exactly* where behavior diverges.

---

## §1 Connection Protocol

Always use the custom PCSX2 build with DebugServer compiled in (port 21512).

```
Step 1: mcp_pcsx2_pcsx2_connect()
        → Must show: DebugServer connected
        → If connection fails: PCSX2 not running or wrong build. Stop.

Step 2: mcp_pcsx2_pcsx2_game_info()
        → Verify the SAME game is loaded as the recomp target.
        → If different game: wrong ISO/disc. Stop.

Step 3: mcp_pcsx2_pcsx2_status()
        → Confirm emulator state (running / paused).
```

**Always connect at session start if PCSX2 is available.** Update `PS2_PROJECT_STATE.md → PCSX2 MCP Status`.

### §1b Known defects in the MCP wrapper — and the direct client

The MCP layer in front of the DebugServer is **not trustworthy for memory or disassembly** (measured 2026-11-20,
PCSX2-MCP v1.0.0):

| Call | Symptom |
|---|---|
| `read_memory format=hexdump` | always errors ("buffer … undefined") |
| `read_memory format=hex` | returns **lagged** data: a 32-byte read comes back as 16 bytes and vice versa, so an address can appear to hold another address's contents |
| `disassemble` | "Cannot read properties of undefined (reading 'map')" |
| `evaluate` | fails or aborts |
| parallel MCP calls | the first is aborted, the second errors — issue **one call at a time** |

Two more traps that look like game bugs:

- **PCSX2 auto-pauses when its window loses focus.** A minimized/hidden window (`-18286,-18286`) makes
  `resume` appear to do nothing: `status` keeps `paused: true` and `cycles` never changes. Restore + focus the
  window first (`ShowWindow(h, 9)`, `SetForegroundWindow(h)`).
- **The DebugServer is newline-delimited JSON over TCP 21512**, not HTTP. A raw `Invoke-WebRequest` fails with
  "protocol violation"; use a socket.

**Use the repo's direct clients instead (they bypass the MCP wrapper):**

```
python tools/ps2dbg.py status                      # alive/paused/pc/cycles
python tools/ps2dbg.py pause | resume | step
python tools/ps2dbg.py read  0x005AA170 0x40       # hex + little-endian u32 words
python tools/ps2dbg.py dump  0x5AA000 0x3000 out.bin   # chunked, byte-exact
python tools/ps2dbg.py dis   0x00100008 20
python tools/ps2dbg.py eval  "pc"                  # also: regs [cat], bp <addr> [cond],
                                                   # watch <addr> <end> [type] [cond], threads, str
python tools/pcsx2_trace.py 150 0x00204240 0x00285E80
                                                   # sets BPs, logs hit#/pc/a0..a3/ra/sp + game fields, resumes
```

`pcsx2_trace.py` understands two Tales-of-Legendia objects so its output is directly comparable with the port's
`[fade]` / `[scene]` probes: the loader at `a0` (`+0x14` mode, `+0x18` arg, `+0x20` overlay) and the scene at
`a0` (`+0x08` entity count).

### §1c Comparing recomp vs emulator — what is real evidence

- **Compare heap objects, not `.bss` constants.** Anything inside the ELF's loaded image (`vaddr` inside
  `PT_LOAD.filesz`) is identical by construction; only runtime-allocated objects prove the port's guest state.
  Check with the ELF program headers before celebrating a match.
- **Align on a guest milestone, not wall time.** Break on a specific function (`tools/pcsx2_trace.py`) in the
  emulator and dump at the same call in the port (`TOL_DUMP_RAM_SCENE` / `TOL_DUMP_RAM`), then diff the two
  dumps byte-for-byte. Wall-clock alignment is meaningless when the port is several times slower.
- **A probe can shadow another probe.** Two `replaceFunction` installs on one address leave only the last one
  active, so "function never called" must be re-checked with an independent probe (e.g. `TOL_TRACE_PC`) before
  it is believed.

---

## §2 Tool Catalog

### Execution Control

| Tool | What It Does | When to Use |
|------|-------------|-------------|
| `pcsx2_pause()` | Halt emulation, return current PC | Before reading registers (values are meaningless while running) |
| `pcsx2_continue()` | Resume until next breakpoint or halt | After inspecting state, to advance to next point of interest |
| `pcsx2_step()` | Execute ONE MIPS instruction | Trace through a function instruction by instruction |
| `pcsx2_step_over()` | Step over JAL/JALR (like "next") | Skip subroutine calls, stay in current function |

### Registers

| Tool | What It Does | When to Use |
|------|-------------|-------------|
| `pcsx2_read_registers()` | Read ALL 128-bit EE registers | Capture CPU state at a breakpoint — THE primary diagnostic tool |
| `pcsx2_read_registers(category=0)` | GPR only | Quick check of `v0`, `a0-a3`, `sp`, `ra` |
| `pcsx2_read_registers(category=2)` | FPR only | Floating-point bugs, graphics math |
| `pcsx2_write_register(index, value)` | Modify a register | Test "what if v0 was X?" hypotheses. **Use with caution.** |
| `pcsx2_evaluate(expression)` | Evaluate MIPS expression | Calculate "v0 + 0x100", "gp + 0x20", dereference pointers |

### Memory

| Tool | What It Does | When to Use |
|------|-------------|-------------|
| `pcsx2_read_memory(addr)` | Read PS2 RAM (hex dump) | Inspect data structures, buffers, game state |
| `pcsx2_read_memory(addr, format="u32_array")` | Read as 32-bit words | Register files, pointer arrays |
| `pcsx2_read_memory(addr, format="ascii")` | Read as text | String buffers, debug messages |
| `pcsx2_write_memory(addr, data)` | Write hex to PS2 RAM | **DANGEROUS.** Only for testing hypotheses. Save state first. |
| `pcsx2_read_string(addr)` | Read null-terminated string | Filename paths, error messages, game text |
| `pcsx2_find_pattern(pattern)` | Search memory for hex pattern | Find data structures, locate specific values. Use `??` for wildcards. |
| `pcsx2_memory_diff(addr, name)` | Snapshot-and-compare | Call once = snapshot. Call again with same name = diff. Shows what changed. |

### Breakpoints & Watchpoints

| Tool | What It Does | When to Use |
|------|-------------|-------------|
| `pcsx2_set_breakpoint(addr)` | Break at address | Stop execution at a specific function or instruction |
| `pcsx2_set_breakpoint(addr, condition="v0 == 0x42")` | Conditional break | Break only when expression is true — essential for rare bugs |
| `pcsx2_set_breakpoint(addr, temporary=True)` | One-shot break | Break once then auto-remove |
| `pcsx2_remove_breakpoint(addr)` | Remove one breakpoint | Clean up after investigation |
| `pcsx2_list_breakpoints()` | List all breakpoints | Verify what's set, avoid forgotten breakpoints |
| `pcsx2_clear_all_breakpoints()` | Remove everything | Session cleanup |
| `pcsx2_set_watchpoint(addr, type="write")` | Break on memory write | Track who writes to a specific address |
| `pcsx2_set_watchpoint(addr, type="onchange")` | Break on value change | Detect unexpected modifications |
| `pcsx2_remove_watchpoint(addr)` | Remove watchpoint | Clean up |
| `pcsx2_list_watchpoints()` | List all watchpoints | Verify what's active |

### Disassembly

| Tool | What It Does | When to Use |
|------|-------------|-------------|
| `pcsx2_disassemble(addr)` | Native MIPS disasm from PCSX2 | Better than Ghidra for runtime — shows actual loaded code, not just ELF |
| `pcsx2_disassemble(addr, count=50)` | More instructions | Larger function context |

### State Management

| Tool | What It Does | When to Use |
|------|-------------|-------------|
| `pcsx2_save_state(slot)` | Save emulator state | **ALWAYS before writing memory or patching.** Slots 0-9. |
| `pcsx2_load_state(slot)` | Restore state | Undo a failed experiment, replay from checkpoint |

### System Info

| Tool | What It Does | When to Use |
|------|-------------|-------------|
| `pcsx2_get_threads()` | List EE/IOP threads | Understand multi-thread state, find stuck threads |
| `pcsx2_get_modules()` | List loaded IOP modules | Verify which I/O modules are active |

---

## §3 A/B Comparison Workflow — The Core Value

This is WHY PCSX2 MCP matters. Instead of guessing what the recompiled code should do, you **observe the real PS2 doing it**, then compare.

```
┌─────────────────────────────────────────────────────────────┐
│            A/B COMPARISON PROTOCOL                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─ SIDE A: PCSX2 (Ground Truth) ──────────────────────┐   │
│  │                                                      │   │
│  │  1. pcsx2_connect()                                  │   │
│  │  2. Advance game to the same point as the crash      │   │
│  │  3. pcsx2_set_breakpoint(target_address)             │   │
│  │  4. pcsx2_continue() → wait for hit                  │   │
│  │  5. pcsx2_read_registers() → save "correct" state    │   │
│  │  6. pcsx2_read_memory(relevant_addrs) → save data    │   │
│  │  7. pcsx2_step() × N → record state at each step     │   │
│  │                                                      │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌─ SIDE B: Recompiled Output ─────────────────────────┐   │
│  │                                                      │   │
│  │  1. Add trace logging in the C++ override/runtime    │   │
│  │     at the same function address                     │   │
│  │  2. Build → Run with timeout → read log              │   │
│  │  3. Extract register/memory state from trace output  │   │
│  │                                                      │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌─ DIFF ──────────────────────────────────────────────┐   │
│  │                                                      │   │
│  │  Compare Side A vs Side B:                           │   │
│  │  • Registers match? → function entry is correct      │   │
│  │  • First register divergence? → THAT is the bug      │   │
│  │  • Memory contents differ? → initialization problem  │   │
│  │  • Return value (v0) wrong? → logic error in body    │   │
│  │                                                      │   │
│  │  First divergence = root cause location.             │   │
│  │  Fix using the 4-tool taxonomy:                      │   │
│  │  TOML / Runtime C++ / Game Override / Recompiler     │   │
│  │                                                      │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### When to Use A/B Comparison

- Crash at a known address but unknown cause
- Function returns wrong value but compiles fine
- DMA/GS output looks wrong but the code "looks right"
- Visual glitch that's hard to reproduce
- Any time stdout alone doesn't explain the bug

### When NOT to Use A/B Comparison

- Build errors (nothing to compare)
- Missing stub (error message tells you exactly what's missing)
- TOML configuration issues (no runtime state to compare)

---

## §4 Common Recipes

### Recipe: "What does this function actually return?"

```
pcsx2_pause()
pcsx2_set_breakpoint("0x<function_end_address>")  # just before JR RA
pcsx2_continue()
# ... hit breakpoint ...
pcsx2_read_registers(category=0)  # check v0 = return value
```

### Recipe: "Is this memory region initialized correctly?"

```
pcsx2_pause()
pcsx2_memory_diff("0x<addr>", name="before_call")  # snapshot BEFORE
pcsx2_set_breakpoint("0x<after_init_address>")
pcsx2_continue()
# ... hit breakpoint ...
pcsx2_memory_diff("0x<addr>", name="before_call")  # diff shows what changed
```

### Recipe: "What are the correct DMA register writes?"

```
pcsx2_set_watchpoint("0x1000A000", type="write")  # DMA channel X CHCR
pcsx2_continue()
# ... watchpoint hit ...
pcsx2_read_registers()  # see what code wrote the DMA register
pcsx2_disassemble(PC)   # see the instruction that triggered it
```

### Recipe: "Conditional breakpoint for a rare bug"

```
# Only break when this function is called with a specific argument
pcsx2_set_breakpoint("0x<addr>", condition="a0 == 0x1234")
pcsx2_continue()
```

### Recipe: "Safe memory experiment"

```
pcsx2_save_state(slot=1)                           # checkpoint
pcsx2_write_memory("0x<addr>", "DEADBEEF")         # test hypothesis
pcsx2_continue()                                    # see what happens
# ... observe result ...
pcsx2_load_state(slot=1)                            # undo if it broke things
```

### Recipe: "Find where a value comes from"

```
# Something writes 0x0000 to address 0x203000. Who?
pcsx2_set_watchpoint("0x203000", type="write", condition="[0x203000] == 0")
pcsx2_continue()
# ... watchpoint hit ...
pcsx2_disassemble(PC)  # the guilty instruction
pcsx2_read_registers() # context around it
```

---

## §5 Safety Rules

1. **ALWAYS `pcsx2_pause()` before `pcsx2_read_registers()`.** Register values are meaningless while the CPU is running — you'll get whatever random state the CPU happens to be in.

2. **ALWAYS `pcsx2_save_state()` before `pcsx2_write_memory()`.** Memory writes can hard-crash the emulation with no way to recover except reloading.

3. **NEVER leave breakpoints set and forget them.** They slow emulation and cause confusion. After each investigation, `pcsx2_list_breakpoints()` → remove spent ones. When done with a session, `pcsx2_clear_all_breakpoints()`.

4. **NEVER `pcsx2_write_register()` without explicit user approval.** Register writes change CPU state and can cause cascading corruption that's impossible to diagnose.

5. **ALWAYS verify the game is at the RIGHT POINT before setting breakpoints.** A breakpoint at address X is useless if the game hasn't loaded that code yet (e.g., overlay not loaded, wrong level).

6. **Use `temporary=True` breakpoints when possible.** One-shot breaks auto-remove. Fewer stale breakpoints = fewer surprises.

7. **Document what you learn.** Every A/B comparison produces knowledge. Write the result (correct register values, expected memory layout) to `PS2_PROJECT_STATE.md → Learned Patterns`.
