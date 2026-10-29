/* pointer locals in direct page slot pairs, and p++ deferred to the next
   sequence point (816-gen.c dp_pend_*): element sizes 1, 2, 4 and a
   struct's, reads, writes, copies with two pointers, p--, the old value
   used as a value (assigned, returned, an argument), p++ in a test, after
   a compare, before && || ?: and a comma, stepped twice in one statement,
   a callee with its own slot pointer, and a for loop stepping it. Each
   walks a local copy of its parameter: locals get the slots */
#include "test.h"

struct item { s16 a, b; u8 f; } items[4];
u16 w[8], dst[8];
u8 b[8];
u32 big[4];
volatile u16 vw[4];
volatile u16 vn = 4;

static u16 sum16(u16 *src, u16 n)
{
    u16 *p = src;
    u16 t = 0, i;
    for (i = 0; i < n; i++)
        t += *p++;
    return t;
}

static u16 sum8(const u8 *src, u16 n)
{
    const u8 *p = src;
    u16 t = 0;
    while (n--)
        t += *p++;              /* one byte: inc */
    return t;
}

static u32 sum32(u32 *src, u16 n)
{
    u32 *p = src;
    u32 t = 0;
    while (n--)
        t += *p++;              /* four bytes */
    return t;
}

static s16 sum_items(struct item *src, u16 n)
{
    struct item *p = src;
    s16 t = 0;
    while (n--) {
        t += p->a - p->b;
        p++;                    /* discarded: a plain step */
    }
    return t;
}

static u16 fields_post(struct item *src, u16 n)
{
    struct item *p = src;
    u16 t = 0;
    while (n--)
        t += (p++)->f;          /* a struct's size, the old value's field */
    return t;
}

static void copy(u16 *dd, const u16 *ss, u16 n)
{
    u16 *d = dd;
    const u16 *s = ss;
    while (n--)
        *d++ = *s++;            /* two pointers, four slots */
}

static void fill_down(u16 *src, u16 n)
{
    u16 *p = src;
    while (n--)
        *p-- = n;               /* p-- */
}

/* run once in a loop: the weight a loop gives puts p in the slots */
static u16 *ret_old(u16 *src)
{
    u16 *p = src;
    while (vn)
        return p++;
    return 0;
}

static u16 deref(u16 *p) { u16 *q = p; return *q++; }  /* a callee with its own */

static u16 as_arg(u16 *src, u16 n)
{
    u16 *p = src;
    u16 t = 0;
    while (n--)
        t += deref(p++);        /* the old value as an argument */
    return t;
}

static u16 to_zero(u16 *src)
{
    u16 *p = src;
    u16 n = 0;
    while (*p++)                /* the test: stepped before the branch */
        n++;
    return n * 100 + (p == &w[5] ? 1 : 0);
}

static u16 cmp_old(u16 *src, u16 *end)
{
    u16 *p = src;
    u16 n = 0;
    while (p++ != end)          /* a compare's flags wait while it steps */
        n++;
    return n;
}

static u16 seq_points(u16 *src)
{
    u16 *p = src;
    u16 r = 0, k;
    for (k = 0; k < 1; k++) {
        if (*p++ == 1 && *p == 2)   /* && : stepped before the right side */
            r += 1;
        r += *p++ == 2 ? *p : 99;   /* ?: */
        r += (p++, *p);             /* comma */
    }
    return r;
}

static u16 twice(u16 *src)
{
    u16 *p = src;
    u16 k, r = 0;
    for (k = 0; k < 1; k++) {
        u16 a = *p++, c = *p++; /* two declarators: two full expressions */
        r = a * 10 + c + *p;
    }
    return r;
}

static u16 for_step(u16 *src, u16 *end)
{
    u16 *p = src;
    u16 t = 0;
    for (; p != end; p++)
        t += *p;
    return t;
}

static u16 vol_reads(volatile u16 *src)
{
    volatile u16 *p = src;
    u16 t = 0, i;
    for (i = 0; i < 4; i++)
        t += *p++;              /* volatile pointee: each read kept */
    return t;
}

int main(void)
{
    u16 i, *q;

    for (i = 0; i < 8; i++) {
        w[i] = i + 1;
        b[i] = (u8) (i * 30);
    }
    for (i = 0; i < 4; i++) {
        items[i].a = (s16) (i * 10);
        items[i].b = (s16) i;
        items[i].f = (u8) (i + 100);
        big[i] = ((u32) (i + 1) << 16) | 1;
        vw[i] = i * 1000;
    }

    CHECK(sum16(w, vn), 1 + 2 + 3 + 4);
    CHECK(sum8(b, 8), 0 + 30 + 60 + 90 + 120 + 150 + 180 + 210);
    CHECK(sum32(big, vn), 0x000A0004ULL);
    CHECK(sum_items(items, vn), 0 + 9 + 18 + 27);
    CHECK(fields_post(items, vn), 100 + 101 + 102 + 103);
    copy(dst, w, 8);
    CHECK(dst[0], 1);
    CHECK(dst[7], 8);
    fill_down(&dst[3], 4);
    CHECK(dst[3], 3);
    CHECK(dst[0], 0);
    CHECK(dst[4], 5);
    CHECK(ret_old(&w[2]) == &w[2], 1);
    CHECK(as_arg(w, vn), 1 + 2 + 3 + 4);
    w[4] = 0;
    CHECK(to_zero(w), 401);
    w[4] = 5;
    CHECK(cmp_old(w, &w[6]), 6);
    CHECK(seq_points(w), 1 + 3 + 4);
    CHECK(twice(w), 10 + 2 + 3);
    CHECK(for_step(w, &w[5]), 1 + 2 + 3 + 4 + 5);
    CHECK(vol_reads(vw), 0 + 1000 + 2000 + 3000);
    q = w;
    q++;
    CHECK(*q, 2);
    return 0;
}
