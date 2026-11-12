#include <stdio.h>
#include <stdint.h>

typedef struct { uint32_t gpr[32]; } Ctx;

/* The generated code uses macros exactly like this one. */
#define SET_GPR_S32(ctx, reg, value) \
    do { (ctx)->gpr[(reg)] = (uint32_t)(value); } while (0)

int main(void)
{
    Ctx ctx = {0};

    /* lui   $v1, 0x39       -> v1 = 0x00390000        */
    SET_GPR_S32(&ctx, 3, (int32_t)((uint32_t)57 << 16));
    printf("after lui        $v1 = 0x%08X\n", ctx.gpr[3]);

    /* addiu $v1, $v1, 0x4378 -> v1 = 0x00394378      */
    SET_GPR_S32(&ctx, 3, ctx.gpr[3] + 17272u);
    printf("after addiu      $v1 = 0x%08X\n", ctx.gpr[3]);
    return 0;
}
