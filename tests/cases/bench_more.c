/* bench_more -- more everyday patterns, timed (see bench_core.c). Each is a
   function called once between t_timer_start and t_timer_stop(id), its
   inputs in volatile globals so nothing folds, its answer checked (the
   expected values worked out apart from the compiler).

     1  copy 64 words, two pointers        while (n--) *d++ = *s++
     2  fill 64 words                      for (i...) buf[i] = v
     3  sum 64 bytes through a pointer     while (n--) t += *p++
     4  8x8 block into a 32x32 tile map    map[(y0 + y) * 32 + x0 + x] = tile
     5  16 actors, through a pointer       p->x += p->dx; p->y += p->dy; p++
     6  switch on 64 opcodes               switch (op[i] & 3) { ... }
     7  clamp 64 signed values             if (v > hi) v = hi; else if (v < lo) ...
     8  count the set bits of 16 words     while (x) { c += x & 1; x >>= 1; }
     9  string length, 60 characters       while (*s++) n++
    10  table lookup through an index      t += tab[idx[i]]
    11  divide 16 values by 10             q += v[i] / 10
    12  16 steps of a fixed-point ease     x += (target - x) >> 2
    13  recursive fib(10), 177 calls       fib(n - 1) + fib(n - 2)
    14  count 16 u32 values over a limit   if (big[i] > limit) c++
*/
#include "test.h"

u16 src[64], dst[64], buf[64], map[32 * 32], tab[16], dv[16];
u8 b8[64], op[64], idx[64];
s16 sin_[64], sout[64];
u16 words[16];
u32 big[16];
struct actor { s16 x, y, dx, dy; u8 flags; } act[16];
const char text[] = "The quick brown fox jumps over the lazy dog, then naps again";
volatile u16 vn64 = 64, vn16 = 16, vtile = 0x1234, vfill = 7;
volatile s16 vlo = -200, vhi = 600, vtarget = 1000;
volatile u32 vlimit = 0x20000ULL;

static void copy_words(u16 *d, const u16 *s, u16 n)
{
    while (n--)
        *d++ = *s++;
}

static void fill_words(u16 v, u16 n)
{
    u16 i;
    for (i = 0; i < n; i++)
        buf[i] = v;
}

static u16 sum_bytes(const u8 *p, u16 n)
{
    u16 t = 0;
    while (n--)
        t += *p++;
    return t;
}

static void block(u16 x0, u16 y0, u16 tile)
{
    u16 x, y;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            map[(y0 + y) * 32 + x0 + x] = tile;
}

static void move_actors(struct actor *p, u16 n)
{
    while (n--) {
        p->x += p->dx;
        p->y += p->dy;
        p++;
    }
}

static u16 run_ops(u16 n)
{
    u16 i, a = 1;
    for (i = 0; i < n; i++) {
        u16 o = op[i];
        switch (o & 3) {
        case 0: a += o; break;
        case 1: a -= o; break;
        case 2: a <<= 1; break;
        default: a ^= 0x55; break;
        }
    }
    return a;
}

static u16 clamp_all(s16 lo, s16 hi, u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++) {
        s16 v = sin_[i];
        if (v > hi)
            v = hi;
        else if (v < lo)
            v = lo;
        sout[i] = v;
        t += v;
    }
    return t;
}

static u16 popcount(u16 n)
{
    u16 i, c = 0;
    for (i = 0; i < n; i++) {
        u16 x = words[i];
        while (x) {
            c += x & 1;
            x >>= 1;
        }
    }
    return c;
}

static u16 length(const char *s)
{
    u16 n = 0;
    while (*s++)
        n++;
    return n;
}

static u16 lookup(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += tab[idx[i]];
    return t;
}

static u16 div10(u16 n)
{
    u16 i, q = 0;
    for (i = 0; i < n; i++)
        q += dv[i] / 10;
    return q;
}

static s16 ease(s16 target, u16 n)
{
    s16 x = 0;
    while (n--)
        x += (target - x) >> 2;
    return x;
}

static u16 fib(u16 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static u16 over(u32 limit, u16 n)
{
    u16 i, c = 0;
    for (i = 0; i < n; i++)
        if (big[i] > limit)
            c++;
    return c;
}

int main(void)
{
    u16 i, r;

    for (i = 0; i < 64; i++) {
        src[i] = i * 3 + 1;
        b8[i] = (u8) (i * 7);
        op[i] = (u8) (i * 5);
        sin_[i] = (s16) (i * 37 - 1000);
        idx[i] = (u8) ((i * 7) & 15);
    }
    for (i = 0; i < 16; i++) {
        tab[i] = i * i;
        dv[i] = i * 1000 + 7;
        words[i] = (i * 0x1111) ^ 0x0F0F;
        big[i] = (u32) i << 14;
        act[i].x = (s16) i;
        act[i].y = 100;
        act[i].dx = 2;
        act[i].dy = -1;
    }

    t_timer_start();
    copy_words(dst, src, vn64);
    t_timer_stop(1);
    CHECK(dst[0], 1);
    CHECK(dst[63], 190);

    t_timer_start();
    fill_words(vfill, vn64);
    t_timer_stop(2);
    CHECK(buf[63], 7);

    t_timer_start();
    r = sum_bytes(b8, vn64);
    t_timer_stop(3);
    CHECK(r, 7200);

    t_timer_start();
    block(4, 3, vtile);
    t_timer_stop(4);
    CHECK(map[3 * 32 + 4], 0x1234);
    CHECK(map[10 * 32 + 11], 0x1234);
    CHECK(map[2 * 32 + 4], 0);
    CHECK(map[3 * 32 + 12], 0);

    t_timer_start();
    move_actors(act, vn16);
    t_timer_stop(5);
    CHECK(act[15].x, 17);
    CHECK(act[0].y, 99);

    t_timer_start();
    r = run_ops(vn64);
    t_timer_stop(6);
    CHECK(r, 53);

    t_timer_start();
    r = clamp_all(vlo, vhi, vn64);
    t_timer_stop(7);
    CHECK(r, 12055);

    t_timer_start();
    r = popcount(vn16);
    t_timer_stop(8);
    CHECK(r, 128);

    t_timer_start();
    r = length(text);
    t_timer_stop(9);
    CHECK(r, 60);

    t_timer_start();
    r = lookup(vn64);
    t_timer_stop(10);
    CHECK(r, 4960);

    t_timer_start();
    r = div10(vn16);
    t_timer_stop(11);
    CHECK(r, 12000);

    t_timer_start();
    r = ease(vtarget, vn16);
    t_timer_stop(12);
    CHECK(r, 988);

    t_timer_start();
    r = fib(10);
    t_timer_stop(13);
    CHECK(r, 55);

    t_timer_start();
    r = over(vlimit, vn16);
    t_timer_stop(14);
    CHECK(r, 7);
    return 0;
}
