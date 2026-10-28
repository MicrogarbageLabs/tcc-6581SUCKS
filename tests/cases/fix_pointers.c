/* a pointer keeps its bank (fork commit 5d16d57): in a static
   initialiser, and cast to a 32-bit integer */
#include "test.h"

static u8 *sp = (u8 *) 0xC80000UL;
u8 *gp = (u8 *) 0x7E2345UL;
static u16 *tab[2] = { (u16 *) 0x410010UL, (u16 *) 0x003400 };
u16 g;
u16 *pg = &g;
u8 arr[4];

int main(void)
{
    u8 *lp;

    CHECK((u32) sp, 0xC80000ULL);
    CHECK((u32) gp, 0x7E2345ULL);
    CHECK((u32) tab[0], 0x410010ULL);
    CHECK((u32) tab[1], 0x003400ULL);
    CHECK((u32) pg == (u32) &g, 1);
    CHECK((u32) &g >> 16, 0x7E);        /* .bss is in $7E */
    CHECK((u32) arr >> 16, 0x7E);
    lp = (u8 *) 0x7F1234UL;
    CHECK((u32) lp + 1, 0x7F1235ULL);

    /* write through a static far pointer, read it back */
    gp = (u8 *) 0x7F8000UL;
    gp[0] = 0x5A;
    gp[1] = 0xA5;
    CHECK(*(volatile u8 *) 0x7F8000UL, 0x5A);
    CHECK(*(volatile u8 *) 0x7F8001UL, 0xA5);
    return 0;
}
