#include <stdio.h>
#include <stdint.h>

/* 0x70000C28 is the first instruction of SLUS_212.01 (entry point 0x00100008). */
int main(void)
{
    uint32_t w = 0x70000C28u;

    uint32_t opcode = (w >> 26) & 0x3Fu;
    uint32_t rs     = (w >> 21) & 0x1Fu;
    uint32_t rt     = (w >> 16) & 0x1Fu;
    uint32_t rd     = (w >> 11) & 0x1Fu;
    uint32_t sa     = (w >>  6) & 0x1Fu;
    uint32_t funct  =  w        & 0x3Fu;

    printf("word    0x%08X\n", w);
    printf("opcode  0x%02X  (%u)\n", opcode, opcode);
    printf("rs      $%u\n", rs);
    printf("rt      $%u\n", rt);
    printf("rd      $%u\n", rd);
    printf("sa      %u\n", sa);
    printf("funct   0x%02X  (%u)  = PADDUW\n", funct, funct);

    /* addiu sp,sp,-0x40 : the 16-bit immediate is signed */
    uint32_t w2 = 0x27BDFFC0u;
    uint32_t raw = w2 & 0xFFFFu;
    int32_t imm = (int32_t)(int16_t)raw;
    printf("0x27BDFFC0 immediate   0x%04X -> %d\n", raw, imm);
    return 0;
}
