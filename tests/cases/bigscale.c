/* a loop over elements too big for an inline multiply: tcc__mul is a call,
   and a call clobbers Y and X. The counter must not be in Y across it, in a
   loop that gets X (the set-up multiplies once) or one that doesn't (the
   body multiplies every pass: the dry run saw no call there). */
#include "test.h"

struct big { u16 v; u8 pad[0x2AB - 2]; } bigs[3];
volatile u16 vn = 3;

static u16 for_x(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += bigs[i].v;
    return t;
}

static u16 for_no_x(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        t += bigs[i].v;
        if (i == 0)
            i = 1;              /* written in the body: no X */
    }
    return t;
}

int main(void)
{
    bigs[0].v = 1;
    bigs[1].v = 20;
    bigs[2].v = 300;
    CHECK(for_x(vn), 321);
    CHECK(for_no_x(vn), 301);
    return 0;
}
