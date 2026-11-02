/* a for loop's counter in Y, indexing elements X can't scale. The dry run
   gives such an index to X and shows no shift or multiply, so Y looked free;
   the real pass then shifted with Y as its count, or called a multiply, and
   the counter was gone. 32 and 64-byte elements, a size that isn't a power
   of two, with a return and a continue, as an LRU's victim search has */
#include "test.h"

#define N 5
static char name[N][32];
static u16 used[N], pin[N];
static u8 big[N][64];
static struct rec { u16 a, b, c; u8 d; } recs[N];   /* 7 bytes */
volatile u16 vc = 100;

static u16 victim(void)
{
    u16 i, best, age, oldest;
    best = N;
    oldest = 0;
    for (i = 0; i < N; i++) {
        if (!name[i][0]) return i;
        if (pin[i]) continue;
        age = (u16) (vc - used[i]);
        if (best == N || age > oldest) {
            best = i;
            oldest = age;
        }
    }
    return best;
}

static u16 sum_big(void)
{
    u16 i, t = 0;
    for (i = 0; i < N; i++)
        t += big[i][3];
    return t;
}

static u16 sum_recs(void)
{
    u16 i, t = 0;
    for (i = 0; i < N; i++) {
        if (i == 3) continue;
        t += recs[i].c + used[i];
    }
    return t;
}

int main(void)
{
    u16 i;
    CHECK(victim(), 0);
    name[0][0] = 'a';
    name[1][0] = 'b';
    CHECK(victim(), 2);
    for (i = 0; i < N; i++) name[i][0] = 'x';
    used[0] = 50; used[1] = 40; used[2] = 60; used[3] = 10; used[4] = 70;
    CHECK(victim(), 3);
    pin[3] = 1;
    CHECK(victim(), 1);
    for (i = 0; i < N; i++) big[i][3] = (u8) (i + 1);
    CHECK(sum_big(), 15);
    for (i = 0; i < N; i++) recs[i].c = (u16) (i * 100);
    CHECK(sum_recs(), 700 + 50 + 40 + 60 + 70);
    return 0;
}
