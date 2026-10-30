## Emotion Engine (EE) - The Main CPU (R5900)

- Core: A custom MIPS core (R5900) designed by Sony and Toshiba.

- Architecture: A mix of MIPS III and MIPS IV instruction sets, heavily augmented with dozens of custom instructions, most notably the MMI (MultiMedia Instructions) for integer SIMD operations.

- Speed: 294.912 MHz.

- Execution: Superscalar design, capable of issuing two instructions per cycle under ideal conditions.

- Registers:

  - GPRs (General Purpose Registers): 32 standard MIPS GPRs, but notably they are 128-bit wide. The full width is used by specific instructions (like MMI). Standard MIPS conventions apply (zero, at, v0-v1, a0-a3, t0-t9, s0-s7, k0-k1, gp, sp, fp, ra).
  - Special Registers: PC (32-bit Program Counter), HI/LO (64-bit multiply/divide result), HI1/LO1 (64-bit, for MULT1/DIV1 type instructions), SA (Shift Amount for QFSRV).

- Caches:

  - 16 KB Instruction Cache (I-Cache).
  - 8 KB Data Cache (D-Cache).
  - Both caches are two-way set associative with 64-byte lines and use an LRF (Least Recently Filled) replacement algorithm. D-cache lines can be locked.

- Scratchpad RAM (SPR): 16 KB of very fast on-chip memory, directly accessible by the CPU (via virtual address 70000000h) and the DMAC. Crucial for performance.

- Coprocessors:

  - COP0 (System Control): Handles exceptions, memory management (MMU/TLB), cache control, timers, and performance counters. It manages virtual memory segments (kuseg, kseg0, kseg1, ksseg, kseg3) and a 48-entry TLB for address translation.
  - COP1 (FPU): A fast single-precision floating-point unit. It deviates from standard IEEE 754: no NaNs/Infinities, denormals are flushed to zero, and rounding is always towards zero (with some quirks). Has 32 single-precision registers (f0-f31).
  - COP2 (VU0 Interface): Acts as the interface to Vector Unit 0 (VU0). The EE can execute specific - COP2 instructions that utilize VU0 directly.

- Memory Management (MMU/TLB): Uses a 48-entry TLB to map virtual addresses (in kuseg, ksseg, kseg3) to physical RAM or Scratchpad. Supports various page sizes (4KB to 16MB). kseg0 and kseg1 provide direct-mapped cached and uncached access respectively.

- Exception Handling: Managed by COP0. Has distinct vectors for different exceptions (Reset, TLB Refill, Interrupt, Debug, etc.) and two levels (Level 1 using EPC, Level 2 using ErrorEPC). Registers like Status, Cause, EPC, ErrorEPC, BadVAddr, BadPAddr are crucial.

- Associated Peripherals (on the EE die/bus): DMAC, Timers (4x 16-bit), GIF, VIF, VU0, VU1, IPU.

## Input/Output Processor (IOP)

- Core: A duplicate of the original PlayStation (PSX) CPU. This implies it's based on a MIPS R3000A core.

- Purpose: Handles slower Input/Output tasks (controllers, memory cards, sound, networking, USB, FireWire, CD/DVD drive control) and provides backward compatibility for PSX games.

- Speed: The document states it's underclocked to the PSX speed (33.8688 MHz) in PSX mode. Its speed in PS2 mode is slightly higher (around 37.5 MHz, though not explicitly stated in this text). It is significantly slower than the EE.

- Architecture: MIPS I instruction set.

- Registers: Standard MIPS R3000A set: 32 general-purpose registers (32-bit), PC, HI/LO.

- Memory: Has its own 2 MB of RAM, separate from the EE's main RAM. Also has access to the shared 4MB BIOS ROM.

- Caches: Standard MIPS R3000A caches (instruction and data, specific sizes not mentioned here but typically 4KB I-Cache, 1KB D-Cache for R3000A). Document mentions cache control at FFFE0000h.

- Coprocessors: Standard R3000A COP0 for basic system control, exceptions, and minimal MMU functionality (segment mapping).

- Memory Management: Simpler than EE. Uses KUSEG, KSEG0, KSEG1 segments for basic mapping.

- Peripherals Controlled: IOP DMA, IOP Timers (PSX + 3x 32-bit), CDVD Drive interface, SIO2 (Serial I/O for controllers/memcards), SPU2 (Sound), DEV9 (Expansion Bay), USB, FireWire. It communicates with the EE via the SIF (Subsystem Interface).

- Interrupts: Has its own interrupt controller (similar to PSX but expanded) managing IRQs from its peripherals (VBLANK, CDVD, DMA, Timers, SIO, SPU2, SIO2, USB, etc.).


## Code representation of R5900

```cpp 
// Represents the programmer-visible register state of the PS2 Emotion Engine (EE) R5900 CPU Core.
// This is suitable as context for emulation or static recompilation.
struct R5900Context
{
    // --- EE Core General Purpose & Special Registers ---

    // EE General Purpose Registers (GPRs). 32 x 128-bit registers (r0-r31). r0 is hardwired to zero.
    // Used for general computation and by MMI instructions.
    __m128i r[32];

    // EE Program Counter (32-bit). Holds the virtual address of the next instruction to fetch/execute.
    uint32_t pc;

    // Emulation/Recompiler state: Tracks the total number of EE instructions executed. Not a hardware register.
    uint64_t insn_count;

    // EE HI/LO registers (64-bit). Store results of multiplication and division. (hi = upper 32 bits, lo = lower 32 bits).
    uint32_t hi, lo;

    // EE secondary HI1/LO1 registers (64-bit). Used by specific MMI MULT1/DIV1 instructions. (hi1 = upper 32 bits, lo1 = lower 32 bits).
    uint32_t hi1, lo1;

    // EE Shift Amount register (32-bit). Used by the MMI QFSRV instruction.
    uint32_t sa;

    // --- VU0 Registers (accessed via EE COP2 instructions in 'Macro Mode') ---
    // Note: VU0 can also run independently in 'Micro Mode'. This section reflects its state accessible *from* the EE.

    // VU0 Vector Float registers (vf00-vf31). 32 x 128-bit (4 floats per register). Accessed via EE COP2 instructions. vf00 = {0.0, 0.0, 0.0, 1.0}.
    __m128 vu0_vf[32];

    // VU0 Vector Integer registers (vi00-vi15). 16 x 16-bit registers. Accessed via EE COP2 instructions. vi00 = 0.
    uint16_t vi[16];

    // VU0 Q register (32-bit float). Holds results from the FDIV unit (DIV, SQRT, RSQRT). Accessed via EE COP2 instructions (e.g., CQC2).
    float vu0_q;

    // VU0 P register (32-bit float). Holds results from the EFU unit (Only VU1 has EFU, but register exists on VU0). Accessed via EE COP2 instructions (e.g., CFC2/CTC2 for status).
    float vu0_p;

    // VU0 I register (32-bit float). Used for immediate values and integer conversions (e.g., MFIR/MTIR). Accessed via EE COP2 instructions.
    float vu0_i;

    // VU0 R register (128-bit vector). Used for random number generation (VRGET/VRNEXT). Accessed via EE COP2 instructions (e.g., CFC2/CTC2).
    __m128 vu0_r;

    // VU0 Status register (16-bit). Holds various status flags (Sticky/Current: Z, S, U, O, I, D). Accessed via EE COP2 instructions (CFC2/CTC2).
    uint16_t vu0_status;

    // VU0 MAC flags register (32-bit). Holds per-element MAC flags (Overflow, Underflow, Sign, Zero for x, y, z, w). Accessed via EE COP2 instructions (CFC2/CTC2).
    uint32_t vu0_mac_flags;

    // VU0 Clipping flags register (24-bit relevant). Holds results of the CLIP instruction (+/-x, +/-y, +/-z). Accessed via EE COP2 instructions (CFC2/CTC2).
    uint32_t vu0_clip_flags;

    // VU0 Microprogram Memory Start Address Register 0 (CMSAR0 - COP2 Reg 8?). Used in macro mode control. Accessed via EE COP2 instructions (CTC2).
    uint32_t vu0_cmsar0;

    // VIF0 FBRST register mirror? Controls VU/VIF resets and stalls. May be mirrored here for convenience when handling EE<->VU0 interaction via COP2.
    uint32_t vu0_fbrst;

    // VU0 Control Flags/Registers? Specific purpose unclear from provided docs, possibly internal recompiler state or less common VU control bits accessed via CFC2/CTC2.
    float vu0_cf[4];

    // --- COP0 System Control Coprocessor Registers ---

    // COP0 Index register ($0). TLB entry access index for TLBR/TLBWI instructions. Selects which TLB entry to read/write.
    uint32_t cop0_index;
    // COP0 Random register ($1). TLB entry access index for TLBWR instruction, decrements automatically. Used for random replacement policy.
    uint32_t cop0_random;
    // COP0 EntryLo0 register ($2). Low part (PFN, C, D, V, G, S) of TLB entry for even virtual pages.
    uint32_t cop0_entrylo0;
    // COP0 EntryLo1 register ($3). Low part (PFN, C, D, V, G) of TLB entry for odd virtual pages.
    uint32_t cop0_entrylo1;
    // COP0 Context register ($4). Contains pointer to page table entry base in memory (used by some OSes on TLB miss).
    uint32_t cop0_context;
    // COP0 PageMask register ($5). Defines the page size for TLB entries (4KB to 16MB).
    uint32_t cop0_pagemask;
    // COP0 Wired register ($6). Specifies the number of "wired" TLB entries that are not subject to random replacement (lower bound for Random reg).
    uint32_t cop0_wired;
    // COP0 BadVAddr register ($8). Stores the virtual address that caused the last address-related exception (TLB miss, TLB invalid, address error).
    uint32_t cop0_badvaddr;
    // COP0 Count register ($9). Free-running 32-bit cycle counter, increments every EE cycle. Used as a high-resolution timer.
    uint32_t cop0_count;
    // COP0 EntryHi register ($10). High part (VPN2, ASID) of TLB entry. Matches against virtual address during TLB lookup.
    uint32_t cop0_entryhi;
    // COP0 Compare register ($11). When Count register equals Compare register, a timer interrupt (INT5) is generated. Writing clears the interrupt.
    uint32_t cop0_compare;
    // COP0 Status register ($12). CPU status and control bits (Interrupt enables IE/EIE, Kernel/Supervisor/User mode KSU, Exception Level EXL, Error Level ERL, Bootstrap Vectors BEV/DEV, Coprocessor Usability CU, Cache Hit CH etc.).
    uint32_t cop0_status;
    // COP0 Cause register ($13). Describes the cause of the last exception (Exception Code ExcCode, Interrupt Pending bits IP, Branch Delay slot flags BD/BD2, Coprocessor Error CE).
    uint32_t cop0_cause;
    // COP0 EPC register ($14). Exception Program Counter. Stores the address to return to after a level 1 exception (using ERET).
    uint32_t cop0_epc;
    // COP0 PRid register ($15). Processor Revision Identifier. Identifies the CPU type and revision (>= 0x59 for EE).
    uint32_t cop0_prid;
    // COP0 Config register ($16). Contains information about cache sizes, architecture features, etc.
    uint32_t cop0_config;
    // COP0 BadPAddr register ($23). Stores the physical address that caused the last bus error exception (if Status.BEM is clear).
    uint32_t cop0_badpaddr;
    // COP0 Debug register ($24). Used for hardware debugging purposes (breakpoints, trace).
    uint32_t cop0_debug;
    // COP0 Perf register ($25). Used for performance counter control and access.
    uint32_t cop0_perf;
    // COP0 TagLo register ($28). Interface for accessing cache tag memory (low part, used with CACHE instruction).
    uint32_t cop0_taglo;
    // COP0 TagHi register ($29). Interface for accessing cache tag memory (high part).
    uint32_t cop0_taghi;
    // COP0 ErrorEPC register ($30). Error Exception Program Counter. Stores the address to return to after a level 2 exception (NMI, Reset, Debug - using ERET when Status.ERL=1).
    uint32_t cop0_errorepc;

    // --- FPU (Floating Point Unit - COP1) Registers ---

    // COP1 Floating-Point Registers (FPRs) (f0-f31). 32 x 32-bit single-precision registers. Note: PS2 FPU has non-standard IEEE 754 behavior (no NaN/Inf, denormals flushed to zero, round-to-zero).
    float f[32];
    // COP1 Floating-Point Control/Status Register (FCR31). Holds FPU condition codes (used by branches), rounding mode (fixed on PS2), and exception flags/enables.
    uint32_t fcr31;
};
```