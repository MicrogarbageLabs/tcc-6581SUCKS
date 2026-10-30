/* a while or do loop's index in X (816-gen.c xl_*), with X following every
   write to the counter: ++ and -- anywhere (the test, the body, either side
   of a read), other writes by shifting from Y, and w[i++] (w - 2,x, X
   stepped already); then the loops that must not get it */
#include "test.h"

struct act { s16 x, y, dx, dy; u8 f; } acts[8];     /* 9 bytes */
u16 w[16];
u8 b[16];
u32 big[8];
volatile u16 vn = 8;

static u16 up(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n) {
        t += w[i];
        i++;
    }
    return t * 100 + i;         /* the counter home after the loop */
}

static u16 down(u16 n)
{
    u16 t = 0;
    while (n--)
        t += w[n];              /* the new value, after the test */
    return t + (n == 0xFFFF ? 1000 : 0);
}

static u16 down_bytes(u16 n)
{
    u16 t = 0;
    while (n--)
        t += b[n];
    return t;
}

static u16 down_struct(u16 n)
{
    u16 t = 0;
    while (n--)
        t += acts[n].x + acts[n].f; /* 9: X steps by adding */
    return t;
}

static u16 pre(u16 n)
{
    u16 t = 0;
    while (--n)
        t += w[n];
    return t;
}

static u16 do_up(u16 n)
{
    u16 i = 0, t = 0;
    do {
        t += b[i];
    } while (++i < n);
    return t;
}

static u16 test_post(u16 n)
{
    u16 i = 0, t = 0;
    while (i++ < n)
        t += w[i];              /* after the step */
    return t;
}

static u16 test_reads(u16 n)
{
    u16 i = 0;
    while (w[i] != n)           /* X in the test */
        i++;
    return i;
}

static u16 two_steps(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n) {
        t += w[i];
        i++;
        t += w[i] * 2;          /* X stepped mid body */
        i++;
    }
    return t;
}

static u16 branches(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n) {
        if (w[i] == 2) {
            i += 2;             /* X from Y */
            continue;
        }
        if (w[i] == 7)
            break;
        t += w[i];
        i++;
    }
    return t * 100 + w[i];
}

static u16 assign(u16 n)
{
    u16 i = n, t = 0;
    while (i != 0) {
        t += w[i];
        i = i >> 1;             /* any value: X from Y */
    }
    return t;
}

static u32 assign_big(u16 n)
{
    u16 i = 0;
    u32 t = 0;
    while (i < n) {
        t += big[i];
        i = i + 3;              /* size 4 */
    }
    return t;
}

static u16 assign_bytes(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n) {
        t += b[i];
        i = i + 3;              /* size 1: tyx */
    }
    return t;
}

static u16 written_for(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n; i += 2) {
        t += w[i];
        if (i == 2)
            i = 5;              /* a for loop's body too */
    }
    return t;
}

static u16 for_no_inc(u16 n)
{
    u16 i, t = 0;
    for (i = n; i-- > 0;)
        t += w[i];
    return t;
}

/* w[i++] and w[i--]: reads, writes, the value on the stack across a branch */
static u16 post_read(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n)
        t += w[i++];
    return t * 100 + i;
}

static u16 post_down(u16 n)
{
    u16 i = n, t = 0;
    while (i != 0)
        t += w[i--];
    return t;
}

static void post_write(u16 n, u16 v)
{
    u16 i = 0;
    while (i < n)
        w[i++] = v;
}

static void post_cond(u16 n, u16 c)
{
    u16 i = 0;
    while (i < n)
        b[i++] = c ? 11 : 22;   /* a branch with the element on the stack */
}

static u16 post_field(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n)
        t += acts[i++].y;       /* 9 bytes, a field */
    return t;
}

static u16 post_for(u16 n)
{
    u16 i, t = 0;
    for (i = 0; i < n;)
        t += b[i++];
    return t;
}

/* the ones that keep their index out of X */
static u16 two_sizes(u16 n)
{
    u16 t = 0;
    while (n--)
        t += w[n] + b[n];
    return t;
}

static u16 two_indexes(u16 n)
{
    u16 i = 0, j = 1, t = 0;
    while (i < n) {
        t += w[i] + w[j];
        i++;
        j++;
    }
    return t;
}

static u16 struct_assign(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n) {
        t += acts[i].x;
        i = i + 2;              /* size 9, no ++: X couldn't follow */
    }
    return t;
}

static u16 post_expr(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n)
        t += w[~i++ & 3];       /* the ++ isn't all of the index */
    return t * 100 + i;
}

static u16 post_scale(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n)
        t += w[i++ + 1];
    return t;
}

static u16 addr_of(u16 n)
{
    u16 i = 0, *p = &i, t = 0;
    while (i < n) {
        t += w[i];
        (*p)++;
    }
    return t;
}

static u16 nested(u16 n)
{
    u16 i = 0, j, t = 0;
    while (i < n) {
        j = 2;
        while (j--)
            t += w[i] + w[j];
        i++;
    }
    return t;
}

static s16 sdown(s16 n)
{
    s16 t = 0;
    while (n >= 0) {
        t += (s16) w[n];
        n--;
    }
    return t;
}

int main(void)
{
    u16 i, t;

    for (i = 0; i < 16; i++) {
        w[i] = i + 1;
        b[i] = (u8) (i * 3);
    }
    for (i = 0; i < 8; i++) {
        acts[i].x = i + 1;
        acts[i].y = i * 2;
        acts[i].f = (u8) i;
        big[i] = (u32) i * 0x10001UL;
    }

    CHECK(up(vn), 3600 + 8);
    CHECK(up(0), 0);
    CHECK(down(vn), 36 + 1000);
    CHECK(down(0), 1000);
    CHECK(down_bytes(4), 0 + 3 + 6 + 9);
    CHECK(down_struct(4), (1 + 2 + 3 + 4) + (0 + 1 + 2 + 3));
    CHECK(pre(4), 2 + 3 + 4);
    CHECK(do_up(4), 0 + 3 + 6 + 9);
    CHECK(do_up(0), 0);
    CHECK(test_post(4), 2 + 3 + 4 + 5);
    CHECK(test_reads(6), 5);
    CHECK(two_steps(4), 1 + 2 * 2 + 3 + 4 * 2);
    CHECK(branches(10), (1 + 4 + 5 + 6) * 100 + 7);
    CHECK(assign(9), 10 + 5 + 3 + 2);
    CHECK(assign_big(8), 0 * 0x10001UL + 3 * 0x10001UL + 6 * 0x10001UL);
    CHECK(assign_bytes(8), 0 + 9 + 18);
    CHECK(written_for(10), 1 + 3 + 8 + 10);
    CHECK(for_no_inc(5), 15);

    CHECK(post_read(vn), 3600 + 8);
    CHECK(post_down(4), 2 + 3 + 4 + 5 + 0 * 0);
    CHECK(post_field(4), 0 + 2 + 4 + 6);
    CHECK(post_for(4), 0 + 3 + 6 + 9);
    post_cond(4, 1);
    CHECK(b[0] + b[3], 22);
    CHECK(b[4], 12);
    post_cond(2, 0);
    CHECK(b[0] + b[1] + b[2], 22 + 22 + 11);
    for (i = 0; i < 16; i++)
        b[i] = (u8) (i * 3);
    post_write(3, 99);
    CHECK(w[0] + w[2], 198);
    CHECK(w[3], 4);
    w[0] = 1;
    w[1] = 2;
    w[2] = 3;

    CHECK(two_sizes(3), (1 + 2 + 3) + (0 + 3 + 6));
    CHECK(two_indexes(3), (1 + 2 + 3) + (2 + 3 + 4));
    CHECK(struct_assign(5), 1 + 3 + 5);
    t = w[~0 & 3] + w[~1 & 3] + w[~2 & 3] + w[~3 & 3];
    CHECK(post_expr(4), t * 100 + 4);
    CHECK(post_scale(3), 2 + 3 + 4);
    CHECK(addr_of(3), 6);
    CHECK(nested(2), 2 * (1 + 2) + (1 + 2) + (1 + 2));
    CHECK(sdown(3), 10);
    return 0;
}
