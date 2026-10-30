/* tccflags: -mcartram */
/* cart RAM with no initialised globals at all: .bss starts where the
   initialised copy would, and crt0's old word-at-a-time copy wrote 2 ROM
   bytes there after the clear even with nothing to copy (the HX-422's T2
   read its error count as $C208). Every .bss variable must start at 0. */
#include "test.h"

u16 first, second;
u8 arr[5];

int main(void)
{
    u16 i, t = 0;
    CHECK(first, 0);
    CHECK(second, 0);
    for (i = 0; i < 5; i++)
        t |= arr[i];
    CHECK(t, 0);
    return 0;
}
