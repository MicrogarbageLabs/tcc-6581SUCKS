/* tccflags: -mcartram */
/* w[i++] with w at the very start of cart RAM: X has stepped already, so
   the access is w - 2,x. As lda.w that is $FFFE,x, which runs into the next
   bank; it has to stay long, whatever the optimiser does to other .bss
   accesses. The compare in the loop, and that w really is at $0000. */
#include "test.h"

u16 w[4];
u8 b[4];
volatile u16 vn;              /* no initial value: w stays first */

static u16 sum(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n)
        t += w[i++];
    return t;
}

static void fill(u16 n)
{
    u16 i = 0;
    while (i < n)
        b[i++] = (u8) n;
}

int main(void)
{
    vn = 4;
    CHECK((u16) (u32) w, 0);
    w[0] = 1;
    w[1] = 10;
    w[2] = 100;
    w[3] = 1000;
    CHECK(sum(vn), 1111);
    fill(vn);
    CHECK(b[0] + b[3], 8);
    return 0;
}
