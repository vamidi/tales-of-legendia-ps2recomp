#include <stdio.h>
#include <stdint.h>

/* Part 0 challenge, reference solution: name the PS2 region of an address. */
static const char *region_of(uint32_t a)
{
    if (a <  0x02000000u)                      return "RDRAM (ELF image / heap / stack)";
    if (a >= 0x10000000u && a < 0x10010000u)   return "EE registers (timers/DMA/INTC)";
    if (a >= 0x11000000u && a < 0x11010000u)   return "VU0/VU1 code+data memory";
    if (a >= 0x12000000u && a < 0x12002000u)   return "GS privileged registers";
    if (a >= 0x1FC00000u && a < 0x20000000u)   return "BIOS ROM";
    if (a >= 0x70000000u && a < 0x70004000u)   return "scratchpad (16 KB)";
    if (a >= 0x80000000u && a < 0xA0000000u)   return "KSEG0 (cached direct map)";
    if (a >= 0xA0000000u && a < 0xC0000000u)   return "KSEG1 (uncached direct map)";
    if (a >= 0xC0000000u)                      return "KSEG2 (TLB mapped)";
    return "unmapped";
}

int main(void)
{
    static const uint32_t probe[] = {
        0x00100008u, 0x00394378u, 0x004E0000u, 0x01FFF000u,
        0x1000F000u, 0x11008000u, 0x12000000u, 0x1FC00000u,
        0x70000000u, 0xA0001234u
    };

    for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        printf("0x%08X  %s\n", probe[i], region_of(probe[i]));
    }
    return 0;
}
