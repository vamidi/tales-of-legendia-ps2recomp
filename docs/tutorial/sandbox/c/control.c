#include <stdio.h>
#include <stdint.h>

/* Why the translator emits goto: a guest PC can resume mid-function. */
static void run(uint32_t resume_pc)
{
    uint32_t pc = resume_pc;
    int steps = 0;

again:
    steps++;
    printf("  execute pc=0x%08X\n", pc);

    if (pc == 0x00100008u) { pc = 0x0010000Cu; goto again; }
    if (pc == 0x0010000Cu) { pc = 0x00100010u; goto again; }

    printf("  stop after %d instruction(s)\n", steps);
}

int main(void)
{
    printf("resume at the entry point:\n");
    run(0x00100008u);

    printf("resume in the middle (as dispatchGuestBranch can):\n");
    run(0x00100010u);
    return 0;
}
