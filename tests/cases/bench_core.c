/* the patterns game code leans on, timed: each block runs N times between
   t_timer_start and t_timer_stop(id), and checks its answer so a faster
   compiler can't get faster by being wrong. Cycles per pass = printed / N.

     1  u16 global array, read          arr16[i]
     2  u16 global array, write         arr16[i] = v
     3  u8 global array, read           arr8[i]
     4  struct array field              act[i].x += act[i].dx
     5  pointer walk                    *p++
     6  a call with 3 u16 args          add3(a, b, c)
     7  32-bit add                      acc32 += v32
     8  local u16 loop counter          for (l = 0; l < N; l++) sum += l
     9  global u16 loop counter         for (g = 0; g < N; g++) sum += g
    10  u16 multiply                    a * b
*/
#include "test.h"

#define N 64

u16 arr16[N];
u8 arr8[N];
struct actor { s16 x, y, dx, dy; u8 flags; } act[N];
volatile u16 va = 3, vb = 5, vc = 7;
volatile u32 v32 = 0x00012345ULL;
u16 sum, g;
u32 acc32;

static u16 add3(u16 a, u16 b, u16 c) { return a + b + c; }

int main(void)
{
    u16 i, l, *p;

    for (i = 0; i < N; i++) {
        arr16[i] = i;
        arr8[i] = (u8) i;
        act[i].x = i;
        act[i].dx = 2;
    }

    sum = 0;
    t_timer_start();
    for (i = 0; i < N; i++)
        sum += arr16[i];
    t_timer_stop(1);
    CHECK(sum, N * (N - 1) / 2);

    t_timer_start();
    for (i = 0; i < N; i++)
        arr16[i] = vc;
    t_timer_stop(2);
    CHECK(arr16[N - 1], 7);

    sum = 0;
    t_timer_start();
    for (i = 0; i < N; i++)
        sum += arr8[i];
    t_timer_stop(3);
    CHECK(sum, N * (N - 1) / 2);

    t_timer_start();
    for (i = 0; i < N; i++)
        act[i].x += act[i].dx;
    t_timer_stop(4);
    CHECK(act[N - 1].x, N + 1);

    sum = 0;
    p = arr16;
    t_timer_start();
    for (i = 0; i < N; i++)
        sum += *p++;
    t_timer_stop(5);
    CHECK(sum, N * 7);

    sum = 0;
    t_timer_start();
    for (i = 0; i < N; i++)
        sum += add3(va, vb, vc);
    t_timer_stop(6);
    CHECK(sum, N * 15);

    acc32 = 0;
    t_timer_start();
    for (i = 0; i < N; i++)
        acc32 += v32;
    t_timer_stop(7);
    CHECK(acc32, 0x00012345ULL * N);

    sum = 0;
    t_timer_start();
    for (l = 0; l < N; l++)
        sum += l;
    t_timer_stop(8);
    CHECK(sum, N * (N - 1) / 2);

    sum = 0;
    t_timer_start();
    for (g = 0; g < N; g++)
        sum += g;
    t_timer_stop(9);
    CHECK(sum, N * (N - 1) / 2);

    sum = 0;
    t_timer_start();
    for (i = 0; i < N; i++)
        sum += va * vb;
    t_timer_stop(10);
    CHECK(sum, N * 15);
    return 0;
}
