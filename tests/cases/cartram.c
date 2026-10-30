/* tccflags: -mcartram */
/* globals in RAM in the cart's ROM area (-mcartram, bank $C8): where each
   kind of variable lands (this file's globals in $C8, reached .w with DB
   there; .wram ones in WRAM $7E; const data in ROM), that .bss starts
   zeroed and initialised globals hold their values, and that they work
   through pointers, indexes, ++ and a struct array. The whole suite also
   runs with TCCFLAGS=-mcartram. */
#include "test.h"

#define BANK_OF(p) ((u16) ((u32) (p) >> 16))

u16 g, gz[8];
static u16 sg;
u16 gi = 1234;
static u16 sgi = 77;
u8 bytes[4] = { 1, 2, 3, 4 };
u8 odd = 9;                     /* an odd size: the copy mustn't spill a byte */
struct pt { s16 x, y; } pts[4];
const u16 ctab[3] = { 10, 20, 30 };
u16 wv __attribute__((section(".wram")));
u8 wtab[16] __attribute__((section(".wram")));
volatile u16 vi = 2;

static void bump(u16 *p) { *p += 5; }

int main(void)
{
    u16 i, t = 0;

    CHECK(__TCC816_CARTRAM__, 0xC8);
    CHECK(BANK_OF(&g), 0xC8);
    CHECK(BANK_OF(&sg), 0xC8);
    CHECK(BANK_OF(&gi), 0xC8);
    CHECK(BANK_OF(pts), 0xC8);
    CHECK(BANK_OF(&wv), 0x7E);
    CHECK(BANK_OF(wtab), 0x7E);
    CHECK(BANK_OF(ctab) != 0xC8 && BANK_OF(ctab) != 0x7E, 1);  /* ROM (PVSnesLib's $00 mirror) */

    CHECK(g, 0);                /* .bss, cleared */
    CHECK(gz[7], 0);
    CHECK(sg, 0);
    CHECK(gi, 1234);            /* copied in */
    CHECK(sgi, 77);
    CHECK(bytes[3], 4);
    CHECK(odd, 9);
    CHECK(ctab[vi], 30);

    g = 5;
    g++;
    sg += g;
    CHECK(sg, 6);
    for (i = 0; i < 8; i++)
        gz[i] = i * 3;
    for (i = 0; i < 8; i++)
        t += gz[i];
    CHECK(t, 84);
    bump(&gi);                  /* through a pointer (long, with the bank) */
    CHECK(gi, 1239);
    pts[vi].x = -4;
    pts[vi].y = pts[vi].x * 2;
    CHECK(pts[2].y, -8);
    wv = 9;
    wtab[vi] = 7;
    CHECK(wv + wtab[2], 16);
    bytes[vi]++;
    CHECK(bytes[2], 4);
    return 0;
}
