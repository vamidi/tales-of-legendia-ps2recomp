#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* A 1 KB stand-in for the console's 32 MB of RAM. */
static uint8_t rdram[1024];

static void store32(uint32_t addr, uint32_t value)
{
    rdram[addr + 0] = (uint8_t)(value & 0xFFu);
    rdram[addr + 1] = (uint8_t)((value >> 8) & 0xFFu);
    rdram[addr + 2] = (uint8_t)((value >> 16) & 0xFFu);
    rdram[addr + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t load32(uint32_t addr)
{
    return (uint32_t)rdram[addr]
         | ((uint32_t)rdram[addr + 1] << 8)
         | ((uint32_t)rdram[addr + 2] << 16)
         | ((uint32_t)rdram[addr + 3] << 24);
}

int main(void)
{
    memset(rdram, 0, sizeof rdram);

    store32(0x10, 0xDEADBEEFu);

    printf("four bytes    %02X %02X %02X %02X\n",
           rdram[0x10], rdram[0x11], rdram[0x12], rdram[0x13]);
    printf("load32       0x%08X\n", load32(0x10));
    printf("sizeof rdram %u bytes\n", (unsigned)sizeof rdram);

    uint8_t *p = rdram;           /* a pointer is just an index with a type */
    p[0x10] = 0x42u;
    printf("after p[0x10]=0x42, load32 = 0x%08X\n", load32(0x10));
    return 0;
}
