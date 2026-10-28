/* tccflags: -mhx422 */
/* -mhx422: 16-bit multiplies on the HX-422 cart's multiplier ($3C30, which
   run816 models), and the 16x16->32 step of 32-bit multiplies. The wanted
   values are folded at compile time, not multiplied by the hardware. */
#include "test.h"

#ifndef __HX422_MUL__
#error "-mhx422 should define __HX422_MUL__"
#endif

volatile u16 a = 1234, b = 56, c = 0xFFFF, d = 0x8001;
volatile s16 sa = -300, sb = 77;
volatile u32 la = 0x00012345ULL, lb = 0x00000ABCULL, lc = 0x12345678ULL;
volatile u16 vi = 3;

struct big { u16 w[13]; } bigs[4];      /* 26 bytes: an index scaled by 26 */

int main(void)
{
    u16 i;

    CHECK(a * b, (1234UL * 56UL) & 0xFFFF);
    CHECK(c * c, 1);
    CHECK(c * d, (0xFFFFULL * 0x8001ULL) & 0xFFFF);
    CHECK((u16) (sa * sb), (u16) (-300 * 77));
    CHECK((s16) (sa * sb), -23100);
    CHECK((u16) (sa * sa), (u16) 90000UL);

    /* constants too dear for shifts and adds */
    CHECK(a * 0x5555, (1234UL * 0x5555UL) & 0xFFFF);
    CHECK(a * 12345, (1234UL * 12345UL) & 0xFFFF);
    CHECK((u16) (sa * -12345), (u16) (-300L * -12345L));
    /* cheap ones stay shifts and adds, and still come out right */
    CHECK(a * 10, 12340);
    CHECK(a * 3, 3702);

    /* 32-bit multiplies (their 16x16->32 step) */
    CHECK(la * lb, 0x00012345ULL * 0x00000ABCULL);
    CHECK(lc * 3, 0x369D0368ULL);
    CHECK(lc * la, (0x12345678ULL * 0x00012345ULL) & 0xFFFFFFFFULL);

    /* an index scaled by a struct size */
    for (i = 0; i < 4; i++)
        bigs[i].w[12] = i * 7;
    i = vi;
    CHECK(bigs[i].w[12], 21);
    return 0;
}
