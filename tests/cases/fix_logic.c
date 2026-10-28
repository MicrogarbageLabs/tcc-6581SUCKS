/* && and || whose right side is itself a jump chain (fork: gtst merges
   chains): 32-bit compares, nested && / ||, !. And constant ?: folds to
   one branch, the other's side effects never happen. */
#include "test.h"

volatile u32 x32, y32;
volatile u16 a, b, c, n;

static u16 t_and32(u32 x, u32 y) { if (x != 0xFFFFFFFFUL && x >= y) return 6; return 0; }
static u16 t_or32(u32 x, u32 y) { if (x == 0xFFFFFFFFUL || x < y) return 6; return 0; }
static u16 t_nest(u16 p, u16 q, u16 r) { return (p || (q && r)) ? 1 : 0; }
static u16 t_nest2(u16 p, u16 q, u16 r) { return ((p && q) || r) ? 1 : 0; }
static u16 t_not(u16 p, u16 q) { return (p && !(q && p)) ? 1 : 0; }
static u16 bump(void) { n++; return 7; }

int main(void)
{
    u16 i, j, k;

    CHECK(t_and32(0xFFFFFFFFUL, 1), 0);
    CHECK(t_and32(5, 1), 6);
    CHECK(t_and32(5, 9), 0);
    CHECK(t_and32(0x10000UL, 0xFFFFUL), 6);
    CHECK(t_or32(0xFFFFFFFFUL, 9), 6);
    CHECK(t_or32(5, 9), 6);
    CHECK(t_or32(9, 5), 0);
    CHECK(t_or32(0x10000UL, 0x10001UL), 6);

    /* every combination of three flags against what C says */
    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++)
            for (k = 0; k < 2; k++) {
                a = i;
                b = j;
                c = k;
                CHECK(t_nest(a, b, c), (i | (j & k)));
                CHECK(t_nest2(a, b, c), ((i & j) | k));
                CHECK(t_not(a, b), (i & !(j & i)));
            }

    /* constant ?: */
    n = 0;
    CHECK(1 ? 5 : bump(), 5);
    CHECK(n, 0);
    CHECK(0 ? 5 : bump(), 7);
    CHECK(n, 1);
    CHECK(((12) < 0) ? -1 : 1, 1);
    CHECK(1 ? 2 : 0x410000UL, 2);
    CHECK(sizeof(1 ? 2 : 0x410000UL), 4);
    CHECK(1 ?: 9, 1);
    return 0;
}
