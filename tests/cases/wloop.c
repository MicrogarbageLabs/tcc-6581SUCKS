/* while and do loops with their most used local in Y (816-gen.c yl_*: no
   increment names a counter, so the dry run counts uses per loop), and
   816-opt's while (n--) test (dey / cpy #$FFFF): n after the loop, n read
   in the body, --n, break and continue, a do loop, nested, a for loop with
   no increment, and the loops that must keep it in memory (a call, a
   goto, the address taken) */
#include "test.h"

u16 w[8];
volatile u16 vn = 5;

static u16 count_down(u16 n)
{
    u16 t = 0;
    while (n--)
        t += 3;
    return t * 10 + (n == 0xFFFF ? 1 : 0);  /* n after the loop: $FFFF */
}

static u16 n_inside(u16 n)
{
    u16 t = 0;
    while (n--)
        t += n;                 /* the new value, in the body */
    return t;
}

static u16 pre_dec(u16 n)
{
    u16 t = 0;
    while (--n)
        t += n;
    return t * 10 + n;
}

static u16 up_to(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n) {
        if (w[i] == 4) {
            i++;
            continue;           /* back to the test */
        }
        if (w[i] == 7)
            break;
        t += w[i];
        i++;
    }
    return t * 100 + i;
}

static u16 do_loop(u16 n)
{
    u16 t = 0;
    do {
        t += n;
    } while (--n);
    return t;
}

static u16 nested(u16 n)
{
    u16 t = 0, k;
    while (n--) {
        k = 3;
        while (k--)
            t += 1;
    }
    return t;
}

static u16 for_no_inc(u16 n)
{
    u16 i, t = 0;
    for (i = n; i-- > 0;)
        t += w[i];
    return t * 10 + (i == 0xFFFF ? 1 : 0);
}

static u16 old_value_used(u16 n)
{
    u16 t = 0, last = 0;
    while ((last = n--) != 0)   /* the old value kept: no dey rule */
        t += last;
    return t;
}

/* in memory */
static u16 id(u16 v) { return v; }
static u16 with_call(u16 n)
{
    u16 t = 0;
    while (n--)
        t += id(n);
    return t;
}

static u16 with_goto(u16 n)
{
    u16 t = 0;
    while (n--) {
        if (n == 2)
            goto out;
        t += n;
    }
out:
    return t * 10 + n;
}

static void dec(u16 *p) { (*p)--; }
static u16 addr_taken(u16 n)
{
    u16 t = 0;
    while (n) {
        t += n;
        dec(&n);
    }
    return t;
}

int main(void)
{
    u16 i;

    for (i = 0; i < 8; i++)
        w[i] = i + 1;

    CHECK(count_down(vn), 151);
    CHECK(count_down(0), 1);
    CHECK(n_inside(vn), 4 + 3 + 2 + 1 + 0);
    CHECK(pre_dec(vn), (4 + 3 + 2 + 1) * 10);
    CHECK(up_to(8), (1 + 2 + 3 + 5 + 6) * 100 + 6);
    CHECK(up_to(3), 6 * 100 + 3);
    CHECK(do_loop(vn), 15);
    CHECK(nested(vn), 15);
    CHECK(for_no_inc(vn), 15 * 10 + 1);
    CHECK(old_value_used(vn), 15);
    CHECK(with_call(vn), 10);
    CHECK(with_goto(vn), (4 + 3) * 10 + 2);
    CHECK(addr_taken(vn), 15);
    return 0;
}
