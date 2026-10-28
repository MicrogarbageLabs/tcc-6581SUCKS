/* every compare, signed and unsigned, on edge values, as a branch and as
   a value; 32-bit compares; and compares whose flags must survive a
   register spill (compares branch straight on the flags since the
   VT_CMP change) */
#include "test.h"

volatile u16 ua[8] = { 0, 1, 2, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF };
volatile s16 sa[8] = { -32768, -32767, -2, -1, 0, 1, 32766, 32767 };
volatile u32 la[6] = { 0, 1, 0xFFFFULL, 0x10000ULL, 0x7FFFFFFFULL, 0xFFFFFFFFULL };
volatile u16 k7 = 7;

static u16 br_u(u16 x, u16 y)
{
    u16 m = 0;
    if (x == y) m |= 1;
    if (x != y) m |= 2;
    if (x < y) m |= 4;
    if (x <= y) m |= 8;
    if (x > y) m |= 16;
    if (x >= y) m |= 32;
    return m;
}

static u16 val_u(u16 x, u16 y)
{
    return (x == y) | ((x != y) << 1) | ((x < y) << 2) | ((x <= y) << 3) | ((x > y) << 4) | ((x >= y) << 5);
}

static u16 br_s(s16 x, s16 y)
{
    u16 m = 0;
    if (x == y) m |= 1;
    if (x != y) m |= 2;
    if (x < y) m |= 4;
    if (x <= y) m |= 8;
    if (x > y) m |= 16;
    if (x >= y) m |= 32;
    return m;
}

static u16 val_s(s16 x, s16 y)
{
    return (x == y) | ((x != y) << 1) | ((x < y) << 2) | ((x <= y) << 3) | ((x > y) << 4) | ((x >= y) << 5);
}

static u16 br_l(u32 x, u32 y)
{
    u16 m = 0;
    if (x == y) m |= 1;
    if (x != y) m |= 2;
    if (x < y) m |= 4;
    if (x <= y) m |= 8;
    if (x > y) m |= 16;
    if (x >= y) m |= 32;
    return m;
}

/* what the answer must be, from the order of the inputs (they're sorted) */
static u16 want(u16 i, u16 j)
{
    if (i == j) return 1 | 8 | 32;
    if (i < j) return 2 | 4 | 8;
    return 2 | 16 | 32;
}

static u16 six(u16 a, u16 b, u16 c, u16 d, u16 e, u16 f) { return a + b + c + d + e + f; }
static u16 eight(u16 a, u16 b, u16 c, u16 d, u16 e, u16 f, u16 g, u16 h)
{
    return a + b + c + d + e + f + g + h * 100;
}

int main(void)
{
    u16 i, j, a, b, c;

    for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++) {
            CHECK(br_u(ua[i], ua[j]), want(i, j));
            CHECK(val_u(ua[i], ua[j]), want(i, j));
            CHECK(br_s(sa[i], sa[j]), want(i, j));
            CHECK(val_s(sa[i], sa[j]), want(i, j));
        }
    for (i = 0; i < 6; i++)
        for (j = 0; j < 6; j++)
            CHECK(br_l(la[i], la[j]), want(i, j));

    /* against constants, both sides of the sign */
    a = k7;
    CHECK(a < 8, 1);
    CHECK(a < 7, 0);
    CHECK((s16) a > -1, 1);
    CHECK((s16) (a - 8) < 0, 1);
    CHECK((u16) (a - 8) > 0x8000, 1);

    /* a compare as the last of many live values: materialising it may
       spill a register while its flags are waiting */
    a = k7; b = k7 + 1; c = k7 + 2;
    CHECK(six(a, b, c, a + b, b + c, a < b), 7 + 8 + 9 + 15 + 17 + 1);
    CHECK(six(a, b, c, a + b, b + c, a > b), 7 + 8 + 9 + 15 + 17 + 0);
    CHECK(six(a * 2, b * 2, c * 2, a + c, (a < b) + (b < c) + (c < a), a + b + c), 14 + 16 + 18 + 16 + 2 + 24);
    /* six computed values in registers before the compare (a compare frees
       its operands, so it finds a register; this keeps it honest) */
    CHECK(eight(a + b, b + c, c + a, a + a, b + b, c + c, a + 1, a < b), 15 + 17 + 16 + 14 + 16 + 18 + 8 + 100);
    CHECK(eight(a + b, b + c, c + a, a + a, b + b, c + c, a + 1, a > b), 15 + 17 + 16 + 14 + 16 + 18 + 8);
    CHECK(eight(a + b, b + c, c + a, a + a, b + b, c + c, (s16) a < -1, (s16) b >= (s16) a), 15 + 17 + 16 + 14 + 16 + 18 + 0 + 100);
    return 0;
}
