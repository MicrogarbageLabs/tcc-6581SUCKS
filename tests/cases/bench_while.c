/* timings for while and do loops over global arrays (t_timer: cycles between
   the calls, in the .out file); 64 passes each. Checks keep them honest. */
#include "test.h"

#define N 64
u16 w[N], dst[N];
u8 by[N];
struct actor { s16 x, y, dx, dy; } acts[N];
volatile u16 vn = N;

static u16 up_w(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n) {
        t += w[i];
        i++;
    }
    return t;
}

static u16 down_w(u16 n)
{
    u16 t = 0;
    while (n--)
        t += w[n];
    return t;
}

static u16 post_w(u16 n)
{
    u16 i = 0, t = 0;
    while (i < n)
        t += w[i++];
    return t;
}

static void copy_w(u16 n)
{
    while (n--)
        dst[n] = w[n];
}

static void clear_b(u16 n)
{
    u16 i = 0;
    do {
        by[i] = 0;
    } while (++i < n);
}

static void step_acts(u16 n)
{
    while (n--) {
        acts[n].x += acts[n].dx;
        acts[n].y += acts[n].dy;
    }
}

static u16 find_w(u16 want)
{
    u16 i = 0;
    while (w[i] != want)
        i++;
    return i;
}

int main(void)
{
    u16 i;
    for (i = 0; i < N; i++) {
        w[i] = i;
        acts[i].x = i;
        acts[i].dx = 2;
        acts[i].dy = -1;
    }
    t_timer_start(); CHECK(up_w(vn), 2016); t_timer_stop(1);
    t_timer_start(); CHECK(down_w(vn), 2016); t_timer_stop(2);
    t_timer_start(); CHECK(post_w(vn), 2016); t_timer_stop(3);
    t_timer_start(); copy_w(vn); t_timer_stop(4);
    CHECK(dst[63], 63);
    t_timer_start(); clear_b(vn); t_timer_stop(5);
    t_timer_start(); step_acts(vn); t_timer_stop(6);
    CHECK(acts[5].x, 7);
    t_timer_start(); CHECK(find_w(63), 63); t_timer_stop(7);
    return 0;
}
