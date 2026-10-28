/*
 *  sst_run.c -- runs sst_fetch.py's slice of the single-step tests on
 *  emu816: each test's one instruction on flat 16 MB of RAM, then the
 *  registers, the RAM it lists and the cycle count against the suite's.
 *
 *    sst_run [-v] file.txt ...      a line per file: tests, state fails,
 *                                   cycle fails; -v prints each failure
 *
 *  Exit 0 when nothing failed.
 *
 *  Public domain (CC0). No warranty.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu816.h"

static uint8_t *ram;
static int verbose;

static uint8_t mread(emu816 *c, uint32_t a) { (void) c; return ram[a]; }
static void mwrite(emu816 *c, uint32_t a, uint8_t v) { (void) c; ram[a] = v; }

typedef struct {
    long r[10];                 /* pc s p a x y dbr d pbr e */
    int n;
    long addr[512], val[512];
} st_t;

static int read_state(char *line, char tag, st_t *s)
{
    char *p = line;
    int i;
    if (*p != tag)
        return 0;
    p++;
    for (i = 0; i < 10; i++)
        s->r[i] = strtol(p, &p, 10);
    s->n = (int) strtol(p, &p, 10);
    if (s->n > 512)
        return 0;
    for (i = 0; i < s->n; i++) {
        s->addr[i] = strtol(p, &p, 10);
        s->val[i] = strtol(p, &p, 10);
    }
    return 1;
}

static const char *RN[10] = {"pc", "s", "p", "a", "x", "y", "dbr", "d", "pbr", "e"};

static int run_file(const char *path, long *nt, long *nsf, long *ncf)
{
    static char line[65536];
    char name[128] = "";
    st_t in, fin;
    long cyc;
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return 1;
    }
    *nt = *nsf = *ncf = 0;
    while (fgets(line, sizeof line, f)) {
        emu816 c;
        long got[10];
        int i, bad = 0, n;
        if (line[0] != 'T')
            continue;
        sscanf(line + 2, "%127s", name);
        if (!fgets(line, sizeof line, f) || !read_state(line, 'I', &in))
            break;
        if (!fgets(line, sizeof line, f) || !read_state(line, 'F', &fin))
            break;
        if (!fgets(line, sizeof line, f) || sscanf(line, "C %ld", &cyc) != 1)
            break;
        memset(&c, 0, sizeof c);
        c.read = mread;
        c.write = mwrite;
        for (i = 0; i < in.n; i++)
            ram[in.addr[i]] = (uint8_t) in.val[i];
        c.pc = in.r[0];
        c.s = in.r[1];
        c.p = in.r[2];
        c.a = in.r[3];
        c.x = in.r[4];
        c.y = in.r[5];
        c.dbr = in.r[6];
        c.d = in.r[7];
        c.pbr = in.r[8];
        c.e = in.r[9];
        {
            /* MVN/MVP move a byte a step; the suite takes the whole move */
            uint16_t at = c.pc;
            uint8_t op = ram[((uint32_t) c.pbr << 16) | c.pc];
            n = emu816_step(&c);
            if (op == 0x44 || op == 0x54)
                while (c.pc == at && n < 100000)
                    n += emu816_step(&c);
        }
        got[0] = c.pc; got[1] = c.s; got[2] = c.p; got[3] = c.a; got[4] = c.x;
        got[5] = c.y; got[6] = c.dbr; got[7] = c.d; got[8] = c.pbr; got[9] = c.e;
        for (i = 0; i < 10; i++)
            if (got[i] != fin.r[i]) {
                if (verbose && !bad)
                    printf("%s %s:", path, name);
                if (verbose)
                    printf(" %s %ld want %ld", RN[i], got[i], fin.r[i]);
                bad = 1;
            }
        for (i = 0; i < fin.n; i++)
            if (ram[fin.addr[i]] != (uint8_t) fin.val[i]) {
                if (verbose && !bad)
                    printf("%s %s:", path, name);
                if (verbose)
                    printf(" [%06lx] %d want %ld", fin.addr[i], ram[fin.addr[i]], fin.val[i]);
                bad = 1;
            }
        if (bad) {
            if (verbose)
                printf("\n");
            (*nsf)++;
        } else if (n != cyc) {
            if (verbose)
                printf("%s %s: cycles %d want %ld\n", path, name, n, cyc);
            (*ncf)++;
        }
        (*nt)++;
        for (i = 0; i < in.n; i++)
            ram[in.addr[i]] = 0;
        for (i = 0; i < fin.n; i++)
            ram[fin.addr[i]] = 0;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    long t = 0, sf = 0, cf = 0;
    int i;
    ram = calloc(1, 1 << 24);
    if (!ram)
        return 2;
    for (i = 1; i < argc; i++) {
        long nt, nsf, ncf;
        if (!strcmp(argv[i], "-v")) {
            verbose = 1;
            continue;
        }
        if (run_file(argv[i], &nt, &nsf, &ncf))
            continue;
        if (nsf || ncf || verbose)
            printf("%s: %ld tests, %ld state, %ld cycles\n", argv[i], nt, nsf, ncf);
        t += nt;
        sf += nsf;
        cf += ncf;
    }
    printf("total: %ld tests, %ld state failures, %ld cycle failures\n", t, sf, cf);
    return (sf || cf) ? 1 : 0;
}
