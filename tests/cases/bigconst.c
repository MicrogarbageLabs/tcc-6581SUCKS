/* const tables too big for bank 0's upper half: each gets a section of its
   own, anywhere in the ROM (tccelf.c). A whole 64 KB bank's worth, a table
   of 40 KB, two small ones that stay put, a static one, and the ways to reach
   them: an index, a variable pointer, a static pointer, sizeof. The test ROM
   is 128 KB, so the bank-sized table can only be in bank 1. */
#include "test.h"

#define T(n) n, n + 1, n + 2, n + 3, n + 4, n + 5, n + 6, n + 7
#define T8(n) T(n), T(n + 8), T(n + 16), T(n + 24), T(n + 32), T(n + 40), T(n + 48), T(n + 56)
#define T64(n) T8(n), T8(n + 64), T8(n + 128), T8(n + 192), T8(n + 256), T8(n + 320), T8(n + 384), T8(n + 448)
#define T512(n) T64(n), T64(n + 512), T64(n + 1024), T64(n + 1536), T64(n + 2048), T64(n + 2560), T64(n + 3072), T64(n + 3584)

/* 32768 words: 8 x 4096 */
const u16 whole[32768u] = { T512(0), T512(4096), T512(8192), T512(12288), T512(16384), T512(20480), T512(24576), T512(28672) };
static const u16 five[5] = { 2, 4, 6, 8, 10 };
const u8 small[4] = { 9, 8, 7, 6 };
static const u16 quarter[4096] = { T512(100) };
const u16 *const whole_ptr = whole;
volatile u16 vi = 32767u;

static u16 sum(const u16 *p, u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i++)
        t += p[i];
    return t;
}

static u16 walk(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n)
        t += whole[i++];
    return t;
}

int main(void)
{
    u16 i = vi;
    u32 a = (u32) whole;

    CHECK(sizeof whole / 2, 32768u);
    CHECK((u16) a, 0);                  /* a bank of its own */
    CHECK(a >> 16 != 0xC0, 1);
    CHECK(whole[0], 0);
    CHECK(whole[1], 1);
    CHECK(whole[i], 32767u);
    CHECK(whole_ptr[i - 1], 32766u);
    CHECK(whole_ptr[300], 300);
    CHECK(quarter[i & 4095], 100 + 4095);
    CHECK(five[4], 10);
    CHECK(small[3], 6);
    CHECK(sum(whole, 10), 45);
    CHECK(sum(quarter + 4090, 6), 6 * 100 + (4090 + 4091 + 4092 + 4093 + 4094 + 4095));
    CHECK(walk(5), 10);
    return 0;
}
