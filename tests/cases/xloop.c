/* a for loop's index in X (816-gen.c xl_*: X = counter * element size for
   the whole loop, stepped with the counter): element sizes 1, 2, 4 and a
   struct's, reads and writes, counting down, continue and break, the
   index's address taken, the test indexing too, an inner while; and the
   loops that must not get it (two sizes, the counter written in the body,
   an index other than the counter, a for loop inside). With them 816-opt's
   byte load (lda M / and #$00ff) on signed, unsigned and volatile bytes,
   and the commutative swap (sta rN / lda M / adc rN -> adc M). */
#include "test.h"

struct act { s16 x, y, dx, dy; u8 f; } acts[8];
u16 w[16];
u8 b[16];
s8 sb[16];
u32 big[8];
volatile u8 vb[4];
volatile u16 vn = 8;

static u16 sum_w(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += w[i];
    return t;
}

static u16 sum_b(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += b[i];
    return t;
}

static s16 sum_sb(u16 n)
{
    u16 i;
    s16 t = 0;
    for (i = 0; i < n; i++)
        t += sb[i];             /* sign extended after the and */
    return t;
}

static u32 sum_big(u16 n)
{
    u16 i;
    u32 t = 0;
    for (i = 0; i < n; i++)
        t += big[i];            /* 4 bytes: X steps by 4 */
    return t;
}

static void step(u16 n)
{
    u16 i;
    for (i = 0; i < n; i++) {
        acts[i].x += acts[i].dx;
        acts[i].y += acts[i].dy;
        acts[i].f |= 0x80;
    }
}

static u16 down(u16 n)
{
    u16 i, t = 0;
    for (i = n; i-- > 0;)       /* no increment: no Y, no X */
        t += w[i];
    for (i = n - 1; i != 0xFFFF; i--)
        w[i] = w[i] + 1;        /* X steps down by 2 */
    return t;
}

static u16 skip_odd(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        if (i & 1)
            continue;           /* the increment steps X too */
        t += w[i];
    }
    return t;
}

static u16 find(u16 n, u16 want)
{
    u16 i;
    for (i = 0; i < n; i++)
        if (w[i] == want)
            break;
    return i;
}

static u16 *addr_of(u16 n)
{
    u16 i, *p = 0;
    for (i = 0; i < n; i++)
        if (w[i] == 5)
            p = &w[i];          /* the address: stx into a register */
    return p;
}

static u16 until_zero(void)
{
    u16 i;
    for (i = 0; w[i]; i++)      /* the test indexes: X in the test too */
        ;
    return i;
}

static u16 inner_while(u16 n)
{
    u16 i, t = 0, k;
    for (i = 0; i < n; i++) {
        k = 2;
        while (k--)
            t += w[i];
    }
    return t;
}

/* the ones that must not get X */
static u16 two_sizes(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += w[i] + b[i];
    return t;
}

static u16 written(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        t += w[i];
        if (i == 2)
            i++;
    }
    return t;
}

static u16 offset(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i + 1 < n; i++)
        t += w[i + 1] - w[i];
    return t;
}

static u16 nested(u16 n)
{
    u16 i, j, t = 0;
    for (i = 0; i < n; i++)
        for (j = 0; j < 2; j++)
            t += w[i] + b[j];
    return t;
}

static u16 vol_bytes(void)
{
    u16 i, t = 0;
    for (i = 0; i < 4; i++)
        t += vb[i];             /* volatile: the byte load stays 8-bit */
    return t;
}

static u16 logic(u16 n)
{
    u16 i, a = 0xFFFF, o = 0, x = 0;
    for (i = 0; i < n; i++) {
        a &= w[i] | 0xFF00;
        o |= w[i];
        x ^= w[i];
    }
    return (a & 0xFF) + o + x;
}

int main(void)
{
    u16 i, t;
    u16 *p;

    for (i = 0; i < 16; i++) {
        w[i] = i + 1;
        b[i] = (u8) (i * 20);
        sb[i] = (s8) (i & 1 ? -(s16) i * 10 : i);
    }
    for (i = 0; i < 8; i++) {
        acts[i].x = i;
        acts[i].y = 100;
        acts[i].dx = 10;
        acts[i].dy = -1;
        acts[i].f = (u8) i;
        big[i] = ((u32) i << 16) + i;
    }
    for (i = 0; i < 4; i++)
        vb[i] = (u8) (200 + i);

    CHECK(sum_w(vn), 36);
    t = 0;
    for (i = 0; i < 8; i++)
        t += (u8) (i * 20);
    CHECK(sum_b(vn), t);
    CHECK(sum_sb(vn), 0 - 10 + 2 - 30 + 4 - 50 + 6 - 70);
    CHECK(sum_big(vn), 0x001C001CULL);
    step(vn);
    CHECK(acts[7].x, 17);
    CHECK(acts[0].y, 99);
    CHECK(acts[5].f, 0x85);
    CHECK(down(vn), 36);
    CHECK(w[0], 2);
    CHECK(w[7], 9);
    CHECK(w[8], 9);
    CHECK(skip_odd(vn), 2 + 4 + 6 + 8);
    CHECK(find(vn, 6), 4);
    CHECK(find(vn, 99), 8);
    p = addr_of(vn);
    CHECK(p == &w[3], 1);
    w[11] = 0;
    CHECK(until_zero(), 11);
    CHECK(inner_while(4), 2 * (2 + 3 + 4 + 5));
    CHECK(two_sizes(4), (2 + 3 + 4 + 5) + (0 + 20 + 40 + 60));
    CHECK(written(vn), 2 + 3 + 4 + 6 + 7 + 8 + 9);
    CHECK(offset(vn), 9 - 2);
    CHECK(nested(3), 2 * (2 + 3 + 4) + 3 * (0 + 20));
    CHECK(vol_bytes(), 200 + 201 + 202 + 203);
    CHECK(logic(4), (2 & 3 & 4 & 5) + (2 | 3 | 4 | 5) + (2 ^ 3 ^ 4 ^ 5));
    return 0;
}
