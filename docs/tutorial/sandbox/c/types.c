#include <stdio.h>
#include <stdint.h>

int main(void)
{
    printf("char        %2u bytes\n", (unsigned)sizeof(char));
    printf("int         %2u bytes\n", (unsigned)sizeof(int));
    printf("long long   %2u bytes\n", (unsigned)sizeof(long long));
    printf("uint8_t     %2u bytes\n", (unsigned)sizeof(uint8_t));
    printf("uint16_t    %2u bytes\n", (unsigned)sizeof(uint16_t));
    printf("uint32_t    %2u bytes\n", (unsigned)sizeof(uint32_t));
    printf("uint64_t    %2u bytes\n", (unsigned)sizeof(uint64_t));
    printf("void *      %2u bytes\n", (unsigned)sizeof(void *));

    uint32_t addr = 0x00394378u;
    uint64_t flag = 1u;
    printf("addr        0x%08X\n", addr);
    printf("flag        %llu\n", (unsigned long long)flag);
    return 0;
}
