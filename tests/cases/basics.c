/* the plain things everything else leans on: integer arithmetic at each
   size, signedness, globals, arrays, structs, pointers, calls */
#include "test.h"

volatile u16 v16a = 1234, v16b = 56;
volatile s16 vs16 = -300;
volatile u32 v32a = 0x12345678ULL, v32b = 0x00010001ULL;
volatile u8 v8 = 200;

u16 arr[8];
struct pt { s16 x, y; u8 tag; } pts[3];

static u16 add3(u16 a, u16 b, u16 c) { return a + b + c; }
static u32 mix(u32 a, u16 b) { return (a << 4) ^ b; }

int main(void)
{
    u16 i;
    u16 *p;

    CHECK(v16a + v16b, 1290);
    CHECK(v16a - v16b, 1178);
    CHECK(v16a * v16b, 69104 & 0xFFFF);
    CHECK(v16a / v16b, 22);
    CHECK(v16a % v16b, 2);
    CHECK(vs16 / 7, -42);
    CHECK(vs16 >> 2, -75);
    CHECK((u16) v8 + 100, 300);
    CHECK((u8) (v8 + 100), 44);

    CHECK(v32a + v32b, 0x12355679ULL);
    CHECK(v32a - v32b, 0x12335677ULL);
    CHECK(v32a >> 8, 0x00123456ULL);
    CHECK(v32a << 4, 0x23456780ULL);
    CHECK(v32a & 0xFFFF0000ULL, 0x12340000ULL);
    CHECK(v32a * 3, 0x369D0368ULL);
    CHECK(v32a / 0x100, 0x00123456ULL);

    for (i = 0; i < 8; i++)
        arr[i] = i * i;
    CHECK(arr[7], 49);
    CHECK(arr[3] + arr[4], 25);

    pts[2].x = -5;
    pts[2].y = 7;
    pts[2].tag = 9;
    CHECK(pts[2].x + pts[2].y, 2);
    CHECK(pts[2].tag, 9);

    p = &arr[2];
    CHECK(p[1], 9);
    p++;
    CHECK(*p, 9);

    CHECK(add3(v16a, v16b, 10), 1300);
    CHECK(mix(v32a, 0x0F0F), 0x23456780ULL ^ 0x0F0F);
    return 0;
}
