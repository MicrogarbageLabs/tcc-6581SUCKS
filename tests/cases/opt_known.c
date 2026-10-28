/* 816-opt's known registers (a load of what A, X or Y holds goes) and
   reused blocks (a computation repeated from the same sources goes): the
   cases where either would be wrong next to ones where they apply.
   TICK is run816's test register at $3F00: one more each read, so a
   volatile read that got dropped shows. */
#include "test.h"

#define TICK (*(volatile u16 *) 0x003F00)

struct act { s16 x, y, dx, dy; u8 f; } acts[8];
u16 arr[16], g, h;
volatile u16 vk = 2;

/* volatile: both reads must happen, in order */
static u16 vol_twice(void)
{
    u16 base = TICK;
    return (TICK == base) ? 1 : (TICK == base + 2) ? 2 : 3;
}

static u16 vol_cmp(void)
{
    u16 n = 0;
    u16 base = TICK;
    if (TICK == base + 1)
        n += 10;
    if (TICK == base + 2)
        n += 1;
    return n;
}

/* two reads with nothing between (A untouched by the compare): the second
   must still happen, or the || sees the first value again */
static u16 vol_or(void)
{
    u16 b = TICK;
    return (TICK == b + 2 || TICK == b + 2) ? 1 : 0;
}

/* the index moves between two uses of act[i] */
static s16 index_moves(u16 i)
{
    s16 a = acts[i].x;
    i++;
    return a + acts[i].x;
}

/* the source is written through a pointer between two computations */
static u16 alias_between(u16 *p)
{
    u16 a = arr[g * 3];
    *p = 1;
    return a * 100 + arr[g * 3];
}

/* a different index, the same shape */
static s16 other_index(u16 i, u16 j)
{
    return acts[i].x + acts[j].x + acts[i].dx;
}

/* the struct shapes the passes are for */
static void step(u16 n)
{
    u16 i;
    for (i = 0; i < n; i++) {
        acts[i].x += acts[i].dx;
        acts[i].y += acts[i].dy;
    }
}

/* a value that looks known but came in by two paths */
static u16 two_paths(u16 c)
{
    u16 r;
    if (c)
        r = g;
    else
        r = h;
    return r + g;
}

int main(void)
{
    u16 i;

    CHECK(vol_twice(), 2);
    CHECK(vol_cmp(), 11);
    CHECK(vol_or(), 1);

    for (i = 0; i < 8; i++) {
        acts[i].x = i * 10;
        acts[i].y = i;
        acts[i].dx = 1;
        acts[i].dy = 2;
    }
    CHECK(index_moves(vk), 20 + 30);
    CHECK(other_index(1, 3), 10 + 30 + 1);

    for (i = 0; i < 16; i++)
        arr[i] = i;
    g = 1;
    CHECK(alias_between(&arr[3]), 300 + 1);

    step(8);
    CHECK(acts[7].x, 71);
    CHECK(acts[7].y, 9);
    CHECK(acts[0].x, 1);

    g = 5;
    h = 7;
    CHECK(two_paths(1), 10);
    CHECK(two_paths(0), 12);
    return 0;
}
