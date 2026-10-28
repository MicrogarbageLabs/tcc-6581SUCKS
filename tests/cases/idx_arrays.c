/* global arrays indexed by a variable (ldx + lda.l sym,x since the
   VT_IDXSYM change): every element size, read, write, read-modify-write,
   ++, the element's address kept and used later, nested indexes, 2D,
   struct fields, initialised arrays (in $7F), and many indexed values
   live at once (spills) */
#include "test.h"

volatile u16 vi = 3, vj = 5, vk = 1;

u8 a8[16];
s8 s8a[16];
u16 a16[16];
s16 s16a[16];
u32 a32[16];
u8 *aptr[4];
u16 grid[4][8];
struct act { s16 x, y, dx, dy; u8 f; } acts[8];
u16 init16[6] = { 10, 20, 30, 40, 50, 60 };   /* initialised: $7F */
u8 map[8] = { 7, 6, 5, 4, 3, 2, 1, 0 };

static u16 sum6(u16 a, u16 b, u16 c, u16 d, u16 e, u16 f) { return a + b + c + d + e + f; }

int main(void)
{
    u16 i, j, k, *p, *q;
    u8 *bp;
    u32 t;

    i = vi;
    j = vj;
    k = vk;

    for (i = 0; i < 16; i++) {
        a8[i] = i * 3;
        s8a[i] = -(s16) i;
        a16[i] = i * 100;
        s16a[i] = -(s16) i * 100;
        a32[i] = 0x10000ULL * i + i;
    }
    i = vi;
    CHECK(a8[i], 9);
    CHECK(s8a[i], -3);
    CHECK(s8a[j] + 10, 5);
    CHECK(a16[i], 300);
    CHECK(s16a[j], -500);
    CHECK(a32[i], 0x30003ULL);
    CHECK(a32[j] + a32[i], 0x80008ULL);

    /* read-modify-write and ++ */
    a16[i] += 7;
    CHECK(a16[i], 307);
    a16[j]++;
    CHECK(a16[j], 501);
    ++a8[i];
    CHECK(a8[i], 10);
    CHECK(a16[i]--, 307);
    CHECK(a16[i], 306);
    a32[i] += 0x10000ULL;
    CHECK(a32[i], 0x40003ULL);
    s8a[j] -= 100;
    CHECK(s8a[j], -105);

    /* the address, kept */
    p = &a16[i];
    q = &a16[j];
    CHECK(*p, 306);
    CHECK(q - p, 2);
    *p = 1234;
    CHECK(a16[3], 1234);
    CHECK((u32) &a16[i] - (u32) a16, 6);
    bp = &a8[j + 1];
    CHECK(*bp, 18);
    aptr[k] = &a8[j];
    CHECK(*aptr[k], 15);

    /* nested and 2D */
    CHECK(a16[map[i]], 400);
    CHECK(map[map[i]], 3);
    for (i = 0; i < 4; i++)
        for (j = 0; j < 8; j++)
            grid[i][j] = i * 10 + j;
    i = vi;
    j = vj;
    CHECK(grid[i][j], 35);
    CHECK(grid[k][j] + grid[i][k], 15 + 31);

    /* struct fields */
    for (i = 0; i < 8; i++) {
        acts[i].x = i;
        acts[i].dx = 10 * i;
        acts[i].f = i + 100;
    }
    i = vi;
    acts[i].x += acts[i].dx;
    CHECK(acts[i].x, 33);
    CHECK(acts[j].f, 105);
    acts[j].y = acts[i].x * 2;
    CHECK(acts[5].y, 66);

    /* initialised ($7F) */
    CHECK(init16[i], 40);
    init16[j] = init16[i] + init16[k];
    CHECK(init16[5], 60);

    /* many live at once */
    CHECK(sum6(a16[k], a16[j], a16[i], init16[k], init16[i], a8[k]), 100 + 501 + 1234 + 20 + 40 + 3);
    t = a32[k] + a32[j] + a32[i] + a32[k + 1] + a32[j + 1] + a32[i + 1];
    CHECK(t, 0x10001ULL + 0x50005ULL + 0x40003ULL + 0x20002ULL + 0x60006ULL + 0x40004ULL);
    return 0;
}
