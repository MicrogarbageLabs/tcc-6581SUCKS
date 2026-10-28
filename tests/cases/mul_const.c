/* multiplying by a constant (shifts and adds, no tcc__mul) against
   multiplying by the same number in a volatile (tcc__mul), for many
   constants and inputs, signed and unsigned; and struct arrays, whose
   index is scaled by the struct's size */
#include "test.h"

volatile u16 xs[8] = { 0, 1, 2, 7, 0x00FF, 0x1234, 0x8001, 0xFFFF };
volatile u16 vc;

struct s10 { s16 x, y, dx, dy; u8 f; } a10[6];    /* 10 bytes */
struct s6 { u16 a, b, c; } a6[6];
struct s14 { u16 w[7]; } a14[6];

#define T(c) do { vc = (c); CHECK((u16) (x * (c)), (u16) (x * vc)); \
                  CHECK((u16) ((s16) x * (c)), (u16) ((s16) x * (s16) vc)); } while (0)

int main(void)
{
    u16 i, x;

    for (i = 0; i < 8; i++) {
        x = xs[i];
        T(3); T(5); T(6); T(10); T(11); T(12); T(13); T(14); T(18); T(20);
        T(24); T(25); T(36); T(40); T(100); T(320); T(1000); T(0x5555);
        T(-1); T(-2); T(-3); T(-10); T(-100); T(0);
    }

    for (i = 0; i < 6; i++) {
        a10[i].x = i * 3;
        a10[i].dx = 100 + i;
        a6[i].c = i + 50;
        a14[i].w[6] = i * 7;
    }
    for (i = 0; i < 6; i++)
        a10[i].x += a10[i].dx;
    CHECK(a10[5].x, 15 + 105);
    CHECK(a10[2].x, 6 + 102);
    CHECK(a6[4].c, 54);
    CHECK(a14[5].w[6], 35);
    CHECK((u16) ((u8 *) &a10[3] - (u8 *) &a10[0]), 30);
    return 0;
}
