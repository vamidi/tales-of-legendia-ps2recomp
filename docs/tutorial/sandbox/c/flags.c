#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* 4 MB stand-in for the console's RDRAM, covering 0x00000000-0x003FFFFF. */
static uint8_t rdram[0x400000];

static void store64(uint32_t addr, uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        rdram[addr + i] = (uint8_t)((value >> (8 * i)) & 0xFFu);
    }
}

static uint64_t load64(uint32_t addr)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)rdram[addr + i] << (8 * i);
    }
    return v;
}

int main(void)
{
    const uint32_t first = 0x00394378u;
    const uint32_t stride = 0x10u;

    memset(rdram, 0, sizeof rdram);
    printf("before: %llu\n", (unsigned long long)load64(first));

    /* FUN_00199938: six 64-bit stores, 16 bytes apart. */
    for (int i = 0; i < 6; i++) {
        uint32_t addr = first + (uint32_t)i * stride;
        store64(addr, 1u);
        printf("sd  1 -> 0x%08X\n", addr);
    }

    printf("after : %llu\n", (unsigned long long)load64(first));
    for (int i = 0; i < 6; i++) {
        uint32_t addr = first + (uint32_t)i * stride;
        printf("  [0x%08X] = %llu\n", addr, (unsigned long long)load64(addr));
    }
    return 0;
}
