/* functions with no locals that forward 32-bit arguments: the compiler
   takes a stack argument's address (tsa / adc #(n + __f_locals + 1)), and
   816-opt's empty-frame removal must rewrite that form too, not only
   n + __f_locals + 1,s */
#include "test.h"

volatile u32 va = 0x12345678ULL, vb = 0x00010002ULL;

static u32 id32(u32 a) { return a; }
static u32 add32(u32 a, u32 b) { return a + b; }
static u32 pass1(u32 a) { return id32(a); }
static u32 pass2(u32 a, u32 b) { return add32(a, b); }
static u32 pass3(u32 a, u32 b, u32 c) { return add32(add32(a, b), c); }

/* the hx_bank_copy_now shape: void, no locals, its arguments passed on */
u32 got_a, got_b, got_c;
void sink3(u32 a, u32 b, u32 c) { got_a = a; got_b = b; got_c = c; }
void fwd3(u32 a, u32 b, u32 c) { sink3(a, b, c); }

int main(void)
{
    CHECK(pass1(va), 0x12345678ULL);
    CHECK(pass2(va, vb), 0x1235567AULL);
    CHECK(pass3(va, vb, vb), 0x1236567CULL);
    fwd3(va, vb, 0xCAFEF00DULL);
    CHECK(got_a, 0x12345678ULL);
    CHECK(got_b, 0x00010002ULL);
    CHECK(got_c, 0xCAFEF00DULL);
    return 0;
}
