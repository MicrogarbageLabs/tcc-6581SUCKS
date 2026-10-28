/* C99 constant types for 16-bit int/long (fork commit 76fee19): a 24-bit
   constant keeps its high word through ?: and through + with a 16-bit
   variable */
#include "test.h"

volatile u16 sel = 1, off = 0x0123;

static u32 pick(u16 c) { return c ? 0x7F0000UL : 0x410000UL; }

int main(void)
{
    u32 p;

    CHECK(pick(sel), 0x7F0000ULL);
    CHECK(pick(0), 0x410000ULL);
    p = 0x410000UL + off;
    CHECK(p, 0x410123ULL);
    CHECK(sizeof(0x7FFF), 2);
    CHECK(sizeof(0x8000), 2);
    CHECK(sizeof(32768), 4);
    CHECK(sizeof(0x10000), 4);
    CHECK(sizeof(1UL), 2);
    CHECK(sizeof(1ULL), 4);
    CHECK(0xFFFFFFFFUL, 0xFFFFFFFFULL);
    CHECK((u32) off & ~3ULL, 0x0120ULL);
    return 0;
}
