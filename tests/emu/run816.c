/*
 *  run816.c -- runs a compiler test ROM on emu816 and reports.
 *
 *    run816 [-q] [-t] [-l cycles] test.sfc
 *
 *  The memory is the HX-422 lean core's (docs/lean-core-architecture.md in
 *  SNES_HX_422): $C0-$FF and $40-$7D are 8 MB of PSRAM with the ROM at 0,
 *  the low 512 KB read-only (a write there fails the test: on the cart it
 *  goes nowhere); $7E-$7F WRAM, its first 8 KB mirrored in the system
 *  banks; $00-$3F/$80-$BF:8000-FFFF mirror PSRAM. The CPU's multiply and
 *  divide registers ($4202-$4206 -> $4214-$4217) work and answer at once,
 *  and so does the HX-422 multiplier (A $3C30, B $3C32; A*B unsigned at
 *  $3C34, signed at $3C38, 32 bits each); $3F00 is a test register that
 *  reads one more each time (its high byte moves it on), for volatile
 *  tests; every other I/O address reads 0.
 *
 *  The test talks through WDM (tests/rt/test.asm). What it prints goes to
 *  stdout, then a last line: PASS, or FAIL and why. A BRK, a write to the
 *  code region, or running past the cycle limit (-l, default 200 million)
 *  fails it. Exit 0 = PASS.
 *
 *    -q   only the last line (and the failed checks)
 *    -t   the cycle count at the end
 *    -x   trace: each instruction's address, opcode and registers to stderr
 *
 *  Public domain (CC0). No warranty.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu816.h"

#define PSRAM_SIZE 0x800000
#define CODE_TOP   0x080000

static uint8_t *psram, wram[0x20000];
static uint8_t io_mula, io_mulb;
static uint16_t io_dividend, io_quot, io_res;
static uint16_t hx_ma, hx_mb;   /* the HX-422 multiplier's operands ($3C30, $3C32) */
static uint16_t tick;           /* $3F00: a test register, one more each 16-bit read */

static int quiet, show_cycles, trace;
static int done, fails, checks;
static const char *why;
static uint32_t bad_write = 0xFFFFFFFF;
static uint64_t t0;

static uint32_t psram_of(uint8_t bank, uint16_t addr)
{
    if (bank >= 0xC0)
        return ((uint32_t) (bank - 0xC0) << 16) | addr;
    if (bank >= 0x40 && bank < 0x7E)
        return 0x400000 + (((uint32_t) (bank - 0x40) << 16) | addr);
    return ((uint32_t) (bank & 0x3F) << 16) | addr;     /* the system banks' upper half ($00-$3F, $80-$BF) */
}

static uint8_t io_read(uint16_t a)
{
    if (a >= 0x3C30 && a <= 0x3C3B) {
        uint32_t u = (uint32_t) hx_ma * hx_mb;
        uint32_t sg = (uint32_t) ((int32_t) (int16_t) hx_ma * (int32_t) (int16_t) hx_mb);
        switch (a) {
        case 0x3C30: return hx_ma & 0xFF;
        case 0x3C31: return hx_ma >> 8;
        case 0x3C32: return hx_mb & 0xFF;
        case 0x3C33: return hx_mb >> 8;
        default:
            if (a < 0x3C38)
                return (u >> (8 * (a - 0x3C34))) & 0xFF;
            return (sg >> (8 * (a - 0x3C38))) & 0xFF;
        }
    }
    switch (a) {
    case 0x3F00: return tick & 0xFF;
    case 0x3F01: { uint8_t v = tick >> 8; tick++; return v; }
    case 0x4214: return io_quot & 0xFF;
    case 0x4215: return io_quot >> 8;
    case 0x4216: return io_res & 0xFF;
    case 0x4217: return io_res >> 8;
    }
    return 0;
}

static void io_write(uint16_t a, uint8_t v)
{
    switch (a) {
    case 0x3C30: hx_ma = (hx_ma & 0xFF00) | v; break;
    case 0x3C31: hx_ma = (hx_ma & 0x00FF) | (v << 8); break;
    case 0x3C32: hx_mb = (hx_mb & 0xFF00) | v; break;
    case 0x3C33: hx_mb = (hx_mb & 0x00FF) | (v << 8); break;
    case 0x4202: io_mula = v; break;
    case 0x4203: io_mulb = v; io_res = io_mula * io_mulb; break;
    case 0x4204: io_dividend = (io_dividend & 0xFF00) | v; break;
    case 0x4205: io_dividend = (io_dividend & 0x00FF) | (v << 8); break;
    case 0x4206:
        if (v) {
            io_quot = io_dividend / v;
            io_res = io_dividend % v;
        } else {
            io_quot = 0xFFFF;
            io_res = io_dividend;
        }
        break;
    }
}

static uint8_t mread(emu816 *c, uint32_t a)
{
    uint8_t bank = a >> 16;
    uint16_t addr = a & 0xFFFF;
    (void) c;
    if (bank == 0x7E || bank == 0x7F)
        return wram[a - 0x7E0000];
    if (bank >= 0x40 && bank < 0x7E)
        return psram[psram_of(bank, addr)];
    if (bank >= 0xC0)
        return psram[psram_of(bank, addr)];
    /* the system banks */
    if (addr < 0x2000)
        return wram[addr];
    if (addr < 0x8000)
        return io_read(addr);
    return psram[psram_of(bank, addr)];
}

static void mwrite(emu816 *c, uint32_t a, uint8_t v)
{
    uint8_t bank = a >> 16;
    uint16_t addr = a & 0xFFFF;
    uint32_t p;
    if (bank == 0x7E || bank == 0x7F) {
        wram[a - 0x7E0000] = v;
        return;
    }
    if ((bank & 0x40) == 0 && addr < 0x8000) {
        if (addr < 0x2000)
            wram[addr] = v;
        else
            io_write(addr, v);
        return;
    }
    p = psram_of(bank, addr);
    if (p < CODE_TOP) {
        if (bad_write == 0xFFFFFFFF)
            bad_write = a;
        c->stopped = 1;
        return;
    }
    psram[p] = v;
}

static uint32_t box32(int off)
{
    uint32_t a = 0x1FF00 + off;
    return wram[a] | (wram[a + 1] << 8) | ((uint32_t) wram[a + 2] << 16) | ((uint32_t) wram[a + 3] << 24);
}

static void hook(emu816 *c, uint8_t n)
{
    switch (n) {
    case 0x01:
        printf("FAIL: t_fail(%u)\n", c->a);
        fails++;
        why = "t_fail";
        done = 1;
        c->stopped = 1;
        break;
    case 0x02:
        if (!quiet)
            putchar(c->a & 0xFF);
        break;
    case 0x03: {
        uint16_t id = wram[0x1FF00] | (wram[0x1FF01] << 8);
        uint32_t got = box32(2), want = box32(6);
        checks++;
        if (got != want) {
            printf("BAD line %u: got $%08lX want $%08lX\n", id, (unsigned long) got, (unsigned long) want);
            fails++;
        }
        break;
    }
    case 0x04:
        t0 = c->cycles;
        break;
    case 0x05:
        if (!quiet)
            printf("cycles %u: %llu\n", c->a, (unsigned long long) (c->cycles - t0 - 2));
        break;
    case 0x06:
        done = 1;
        c->stopped = 1;
        break;
    case 0x07:
        why = "BRK, COP or an interrupt (t_crash)";
        done = 1;
        c->stopped = 1;
        break;
    }
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    unsigned long long limit = 200000000ULL;
    FILE *f;
    long n;
    emu816 c;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else if (!strcmp(argv[i], "-t"))
            show_cycles = 1;
        else if (!strcmp(argv[i], "-x"))
            trace = 1;
        else if (!strcmp(argv[i], "-l") && i + 1 < argc)
            limit = strtoull(argv[++i], NULL, 0);
        else
            path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: run816 [-q] [-t] [-l cycles] test.sfc\n");
        return 2;
    }
    psram = calloc(1, PSRAM_SIZE);
    f = fopen(path, "rb");
    if (!psram || !f) {
        perror(path);
        return 2;
    }
    n = (long) fread(psram, 1, CODE_TOP, f);
    fclose(f);
    if (n <= 0) {
        fprintf(stderr, "%s: empty\n", path);
        return 2;
    }

    memset(&c, 0, sizeof c);
    c.read = mread;
    c.write = mwrite;
    c.wdm = hook;
    emu816_reset(&c);
    while (!c.stopped && c.cycles < limit) {
        uint8_t op = mread(&c, ((uint32_t) c.pbr << 16) | c.pc);
        if (op == 0x00) {
            why = "BRK: ran into empty ROM";
            break;
        }
        if (trace)
            fprintf(stderr, "%02X:%04X %02X  A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X P=%02X\n",
                    c.pbr, c.pc, op, c.a, c.x, c.y, c.s, c.d, c.dbr, c.p);
        emu816_step(&c);
    }
    if (!done && !why) {
        if (bad_write != 0xFFFFFFFF)
            why = "a write to the code region";
        else if (c.cycles >= limit)
            why = "the cycle limit";
        else
            why = "STP or WAI before t_done";
    }
    if (show_cycles)
        printf("cycles: %llu\n", (unsigned long long) c.cycles);
    if (why) {
        printf("FAIL: %s at $%02X:%04X", why, c.pbr, c.pc);
        if (bad_write != 0xFFFFFFFF)
            printf(" (wrote $%06lX)", (unsigned long) bad_write);
        printf(", %d of %d checks bad\n", fails, checks);
        return 1;
    }
    if (fails) {
        printf("FAIL: %d of %d checks bad\n", fails, checks);
        return 1;
    }
    printf("PASS: %d checks\n", checks);
    return 0;
}
