/* a pointer cast from a u32 computed in registers: the pair has to become
   one register and its high half, or the bank is whatever was left there.
   A u32 sitting in memory always worked. Banks differ on purpose, so a
   stale high half can't match by luck */
#include "test.h"

volatile u16 vb = 0x7F, vo = 4, vn = 2;
volatile u32 vr = 0x7F0100UL;
u16 *p;
u8 *q;

static u16 *give(u32 a) { return (u16 *) (a + 2); }
static u16 take(u16 *a) { return *a; }
static u16 *implied(u32 a) { u16 *r = (void *) a; return r + 1; }

int main(void)
{
    u32 l;
    u16 *w = (u16 *) 0x7F0100UL;
    u16 i;

    for (i = 0; i < 8; i++) w[i] = 0xA000 + i;
    ((u16 *) 0x7E8100UL)[0] = 0x1234;

    /* a shift */
    p = (u16 *) ((u32) vb << 16);
    CHECK((u32) p, 0x7F0000UL);
    p = (u16 *) ((u32) (vb - 1) << 16);
    CHECK((u32) p, 0x7E0000UL);
    p = (u16 *) (((u32) (vb - 1) << 16) | 0x8100);
    CHECK(*p, 0x1234);

    /* a sum, in each order */
    p = (u16 *) (vr + 2);
    CHECK((u32) p, 0x7F0102UL);
    CHECK(*p, 0xA001);
    p = (u16 *) (vr + vo);
    CHECK(*p, 0xA002);
    p = (u16 *) (vo + vr);
    CHECK(*p, 0xA002);
    p = (u16 *) (0x7F0100UL + vo);
    CHECK(*p, 0xA002);

    /* through the cast straight to a load or store */
    CHECK(*(u16 *) (vr + 6), 0xA003);
    *(u16 *) (vr + 8) = 0xBEEF;
    CHECK(w[4], 0xBEEF);
    q = (u8 *) (vr + 9);
    CHECK(*q, 0xBE);

    /* arguments, returns, an implied cast */
    CHECK(take((u16 *) (vr + 2)), 0xA001);
    CHECK(*give(vr), 0xA001);
    CHECK(*implied(vr), 0xA001);

    /* a local, and a pair that stays in registers across the cast */
    l = vr + 4;
    p = (u16 *) l;
    CHECK(*p, 0xA002);
    p = (u16 *) (l + vn);
    CHECK(*p, 0xA003);

    /* and back */
    l = (u32) (p + 1);
    CHECK(l, 0x7F0108UL);
    CHECK((u32) (u16 *) (l - 2), 0x7F0106UL);
    return 0;
}
