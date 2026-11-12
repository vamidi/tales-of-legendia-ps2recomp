#include <stdio.h>
#include <stdint.h>

int main(void)
{
    uint32_t max = 0xFFFFFFFFu;
    uint32_t wrapped = max + 1u;
    printf("0xFFFFFFFF + 1          = 0x%08X\n", wrapped);

    uint32_t sp = 0x00100000u;
    uint32_t frame = 0x40u;
    printf("0x00100000 - 0x40       = 0x%08X\n", sp - frame);

    int32_t minus_one = -1;
    uint32_t as_unsigned = (uint32_t)minus_one;
    printf("-1 as uint32_t          = 0x%08X\n", as_unsigned);

    uint32_t mask = 0x01FFFFFFu;
    uint32_t uncached = 0xA0001234u;
    printf("0xA0001234 & 0x01FFFFFF = 0x%08X\n", uncached & mask);
    return 0;
}
