#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Shaped like the runtime's R5900Context: registers are 128 bits wide. */
typedef struct {
    uint8_t  gpr[32][16];   /* 32 GPRs x 16 bytes  = 512 */
    uint32_t pc;            /* offset 512              */
    uint32_t hi;            /* offset 516              */
    uint32_t lo;            /* offset 520              */
} Cpu;

int main(void)
{
    printf("sizeof(Cpu)        %u\n", (unsigned)sizeof(Cpu));
    printf("offsetof(pc)       %u\n", (unsigned)offsetof(Cpu, pc));
    printf("offsetof(hi)       %u\n", (unsigned)offsetof(Cpu, hi));
    printf("offsetof(lo)       %u\n", (unsigned)offsetof(Cpu, lo));

    Cpu cpu;
    memset(&cpu, 0, sizeof cpu);

    cpu.pc = 0x00100008u;

    Cpu *p = &cpu;                 /* p holds the ADDRESS of cpu      */
    printf("cpu.pc            0x%08X\n", cpu.pc);
    printf("p->pc             0x%08X\n", p->pc);      /* same thing */
    printf("(*p).pc           0x%08X\n", (*p).pc);   /* same thing */
    return 0;
}
