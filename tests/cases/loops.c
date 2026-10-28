/* for loops with the increment after the body (replayed tokens): every
   clause there or not, continue and break, nesting, increments with
   calls, commas, macros and side effects; and while / do for company */
#include "test.h"

volatile u16 vn = 10;
u16 calls, g;

static u16 step(void) { calls++; return 2; }
#define NEXT(x) ((x) += 3)
#define FOR_EACH(v, n) for ((v) = 0; (v) < (n); (v)++)

int main(void)
{
    u16 i, j, n, s, t;

    n = vn;

    s = 0;
    for (i = 0; i < n; i++) s += i;
    CHECK(s, 45);
    CHECK(i, 10);

    s = 0;
    for (i = 0; i < n; ++i) { if (i == 3) continue; s += i; }
    CHECK(s, 42);

    s = 0;
    for (i = 0; i < n; i++) { if (i == 6) break; s += i; }
    CHECK(s, 15);
    CHECK(i, 6);

    /* no condition: break ends it */
    s = 0;
    for (i = 0;; i++) { if (i > 4) break; s += i; }
    CHECK(s, 10);

    /* no increment: continue goes to the test */
    s = 0;
    for (i = 0; i < n;) { i++; if (i & 1) continue; s += i; }
    CHECK(s, 30);

    /* no init, no increment */
    i = 0;
    s = 0;
    for (; i < 5;) s += i++;
    CHECK(s, 10);

    /* increments with calls, commas, macros, parentheses */
    calls = 0;
    s = 0;
    for (i = 0; i < n; i += step()) s++;
    CHECK(s, 5);
    CHECK(calls, 5);
    s = 0;
    for (i = 0, j = 100; i < n; i++, j -= 10) s += j;
    CHECK(s, 550);
    s = 0;
    for (i = 0; i < n; NEXT(i)) s++;
    CHECK(s, 4);
    s = 0;
    for (i = 0; i < n; i = (i + (1 * 2))) s++;
    CHECK(s, 5);
    s = 0;
    FOR_EACH(i, n) s += 2;
    CHECK(s, 20);

    /* nested, with continue and break in each */
    s = 0;
    for (i = 0; i < 6; i++) {
        if (i == 1) continue;
        for (j = 0; j < 6; j++) {
            if (j == 2) continue;
            if (j == 4) break;
            s += i * 10 + j;
        }
        if (i == 4) break;
    }
    CHECK(s, (0 + 1 + 3) + (60 + 1 + 3) + (90 + 1 + 3) + (120 + 1 + 3));

    /* a global counter, a down count, an empty body */
    s = 0;
    for (g = 0; g < n; g++) s += g;
    CHECK(s, 45);
    t = 0;
    for (i = n; i > 0; i--) t += i;
    CHECK(t, 55);
    for (i = 0; i < n; i++)
        ;
    CHECK(i, 10);

    /* while and do, unchanged */
    s = 0;
    i = 0;
    while (i < n) { s += i; i++; }
    CHECK(s, 45);
    s = 0;
    i = 0;
    do { s += i; } while (++i < n);
    CHECK(s, 45);
    return 0;
}
