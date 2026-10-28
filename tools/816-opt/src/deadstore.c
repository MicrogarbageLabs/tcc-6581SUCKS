/*---------------------------------------------------------------------------------

	deadstore.c -- remove stores to tcc's pseudo-registers whose value is never
	read, by liveness over the whole file's control flow (816-tcc fork).

	816-opt's own store elimination looks a few lines ahead in straight code;
	this follows branches, so a store before a loop's test, or one whose only
	reader was optimised away, goes too.

	The rules, all on the side of keeping a store when unsure:
	  - a line reads every tcc__rN / tcc__fN it names; [tcc__rN] reads the bank
	    word tcc__rNh as well
	  - only "sta/stz/stx/sty(.b) tcc__X" writes X whole, and only with the
	    register 16 bits (a store under sep #$20 or #$10 is a partial write:
	    a read here)
	  - jsr/jsl to C code reads no pseudo-register (its arguments are on the
	    stack) and leaves none live (816-tcc saves what it needs before a
	    call); a call to a tcc__ helper reads them all
	  - rtl/rts/rti leave the return registers live (r0, r0h, r1, r1h, f*);
	    a jump to a label not in the file, an indirect jump, or a section's
	    end leaves everything live
	  - a store on a labelled line, or one marked DON'T OPTIMIZE, stays

	Public domain (CC0). No warranty.

***************************************************************************/

#include "deadstore.h"

#include <stdint.h>

typedef uint64_t regset;
#define ALL ((regset) ~0ULL)

typedef struct {
    regset use, def;
    int nsucc;
    int succ[2];
    int exit;               /* no successor: out is exitset */
    regset exitset;
    int store;              /* a whole store, deletable when dead */
    int keep;
} line_t;

/* tcc__r0..r10 (+h), tcc__f0..f3 (+h) as bits */
static int reg_bit(const char *p, const char **end)
{
    int f, n = 0, h = 0, digits = 0;
    if (strncmp(p, "tcc__", 5))
        return -1;
    p += 5;
    if (*p != 'r' && *p != 'f')
        return -1;
    f = (*p == 'f');
    p++;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (!digits || n > 15)
        return -1;
    if (*p == 'h') {
        h = 1;
        p++;
    }
    /* a longer name (tcc__registers, tcc__r5x) is not a register */
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')
        return -1;
    if (end)
        *end = p;
    return (f ? 32 : 0) + n * 2 + h;
}

static regset regs_in(const char *s)
{
    regset m = 0;
    const char *p = s, *e;
    while ((p = strstr(p, "tcc__")) != NULL) {
        int b = reg_bit(p, &e);
        if (b >= 0) {
            m |= (regset) 1 << b;
            /* a long pointer [tcc__rN] reads its bank too */
            if (p > s && p[-1] == '[' && !(b & 1))
                m |= (regset) 1 << (b + 1);
            p = e;
        } else
            p += 5;
    }
    return m;
}

static regset ret_regs(void)
{
    regset m = 0;
    int b;
    for (b = 0; b < 4; b++)
        m |= (regset) 1 << b;   /* r0 r0h r1 r1h */
    for (b = 32; b < 64; b++)
        m |= (regset) 1 << b;   /* the float registers */
    return m;
}

/* the text without a trailing comment, trimmed at the end */
static void code_of(const char *s, char *out, size_t n)
{
    size_t i = 0;
    while (s[i] && s[i] != ';' && i + 1 < n) {
        out[i] = s[i];
        i++;
    }
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t'))
        i--;
    out[i] = 0;
}

/* an anonymous label at the start of a line ("+", "++", "-", "+ dex"):
   its length, or 0 */
static int anon_label(const char *c)
{
    int n = 0;
    char ch = c[0];
    if (ch != '+' && ch != '-')
        return 0;
    while (c[n] == ch)
        n++;
    if (c[n] == 0 || c[n] == ' ' || c[n] == '\t')
        return n;
    return 0;
}

static int is_named_label(const char *c)
{
    size_t n = strlen(c);
    return n > 1 && c[n - 1] == ':' && !strchr(c, ' ');
}

static int find_label(dynArray *f, const char *name)
{
    char want[512], code[1024];
    size_t i;
    snprintf(want, sizeof want, "%s:", name);
    for (i = 0; i < f->used; i++) {
        code_of(f->arr[i], code, sizeof code);
        if (!strcmp(code, want))
            return (int) i;
    }
    return -1;
}

/* "+", "++" forward to the next line with that label; "-" backward */
static int find_anon(dynArray *f, int from, const char *ref)
{
    char code[1024];
    int n = (int) strlen(ref), i;
    char ch = ref[0];
    if (ch == '+') {
        for (i = from + 1; i < (int) f->used; i++) {
            code_of(f->arr[i], code, sizeof code);
            if (anon_label(code) == n && code[0] == ch)
                return i;
        }
    } else {
        for (i = from; i >= 0; i--) {
            code_of(f->arr[i], code, sizeof code);
            if (anon_label(code) == n && code[0] == ch)
                return i;
        }
    }
    return -1;
}

static int is_anon_ref(const char *s)
{
    int n = 0;
    char ch = s[0];
    if (ch != '+' && ch != '-')
        return 0;
    while (s[n] == ch)
        n++;
    return s[n] == 0;
}

size_t deadStores(dynArray *file)
{
    size_t n = file->used, i, removed = 0;
    line_t *L;
    int m8 = 0, x8 = 0, changed;
    regset *in, *out;
    char code[MAXLEN_LINE];

    if (!n)
        return 0;
    L = calloc(n, sizeof *L);
    in = calloc(n, sizeof *in);
    out = calloc(n, sizeof *out);
    if (!L || !in || !out)
        fatal("malloc-deadstore");

    for (i = 0; i < n; i++) {
        line_t *l = &L[i];
        char *c, mn[8] = "", *op;
        int lab, k;

        code_of(file->arr[i], code, sizeof code);
        c = code;
        l->keep = strstr(file->arr[i], "DON'T OPTIMIZE") != NULL;
        l->nsucc = 1;
        l->succ[0] = (int) i + 1;

        if (c[0] == 0) {
            continue;
        }
        if (is_named_label(c)) {
            continue;
        }
        if (c[0] == '.') {
            if (!strncmp(c, ".ENDS", 5)) {      /* code doesn't run on into what follows */
                l->exit = 1;
                l->exitset = ALL;
                l->nsucc = 0;
            }
            continue;
        }
        lab = anon_label(c);
        if (lab) {
            l->keep = 1;                        /* never drop a labelled line */
            c += lab;
            while (*c == ' ' || *c == '\t')
                c++;
            if (!*c)
                continue;
        }

        /* the mnemonic's base (lda.b -> lda) and the operand */
        for (k = 0; k < 3 && c[k]; k++)
            mn[k] = (char) tolower((unsigned char) c[k]);
        mn[k] = 0;
        op = strchr(c, ' ');
        while (op && (*op == ' ' || *op == '\t'))
            op++;
        if (!op)
            op = c + strlen(c);

        /* register widths, as the code runs down */
        if (!strcmp(mn, "sep") || !strcmp(mn, "rep")) {
            unsigned v = 0;
            const char *h = strchr(op, '$');
            if (h)
                v = (unsigned) strtoul(h + 1, NULL, 16);
            else if (*op == '#')
                v = (unsigned) strtoul(op + 1, NULL, 10);
            if (v & 0x20)
                m8 = (mn[0] == 's');
            if (v & 0x10)
                x8 = (mn[0] == 's');
        }

        if ((!strcmp(mn, "sta") || !strcmp(mn, "stz") || !strcmp(mn, "stx") || !strcmp(mn, "sty"))
            && strncmp(op, "tcc__", 5) == 0) {
            const char *e;
            int b = reg_bit(op, &e);
            int narrow = (mn[2] == 'a' || mn[2] == 'z') ? m8 : x8;
            if (b >= 0 && *e == 0 && !narrow) {
                l->def = (regset) 1 << b;
                l->store = 1;
                continue;
            }
        }

        l->use = regs_in(op);

        /* calls */
        if (!strcmp(mn, "jsr") || !strcmp(mn, "jsl")) {
            if (!strncmp(op, "tcc__", 5))
                l->use = ALL;
            l->def = ALL;
            continue;
        }

        /* returns */
        if (!strcmp(mn, "rtl") || !strcmp(mn, "rts") || !strcmp(mn, "rti")) {
            l->exit = 1;
            l->exitset = ret_regs();
            l->nsucc = 0;
            continue;
        }
        if (!strcmp(mn, "stp")) {
            l->exit = 1;
            l->exitset = 0;
            l->nsucc = 0;
            continue;
        }

        /* jumps and branches */
        {
            int cond = mn[0] == 'b' && strcmp(mn, "bra") && strcmp(mn, "brl") && strcmp(mn, "bit")
                       && strcmp(mn, "brk");
            int uncond = !strcmp(mn, "bra") || !strcmp(mn, "brl") || !strcmp(mn, "jmp") || !strcmp(mn, "jml");
            if (cond || uncond) {
                int t = -1;
                if (strchr(op, '(') || strchr(op, '['))
                    t = -1;             /* indirect */
                else if (is_anon_ref(op))
                    t = find_anon(file, (int) i, op);
                else
                    t = find_label(file, op);
                if (uncond) {
                    if (t < 0) {
                        l->exit = 1;
                        l->exitset = ALL;
                        l->nsucc = 0;
                    } else {
                        l->succ[0] = t;
                    }
                } else {
                    if (t < 0) {
                        l->use = ALL;   /* unknown target: everything may be read there */
                    } else {
                        l->nsucc = 2;
                        l->succ[1] = t;
                    }
                }
            }
        }
    }

    /* liveness, backwards to a fixpoint */
    do {
        changed = 0;
        for (i = n; i-- > 0;) {
            line_t *l = &L[i];
            regset o = 0, ni;
            int s;
            if (l->exit)
                o = l->exitset;
            for (s = 0; s < l->nsucc; s++) {
                int t = l->succ[s];
                if (t >= (int) n)
                    o = ALL;            /* off the end */
                else
                    o |= in[t];
            }
            ni = l->use | (o & ~l->def);
            if (o != out[i] || ni != in[i]) {
                out[i] = o;
                in[i] = ni;
                changed = 1;
            }
        }
    } while (changed);

    /* drop the dead stores */
    {
        size_t w = 0;
        for (i = 0; i < n; i++) {
            if (L[i].store && !L[i].keep && !(out[i] & L[i].def)) {
                free(file->arr[i]);
                removed++;
                continue;
            }
            file->arr[w++] = file->arr[i];
        }
        file->used = w;
    }

    free(L);
    free(in);
    free(out);
    return removed;
}
