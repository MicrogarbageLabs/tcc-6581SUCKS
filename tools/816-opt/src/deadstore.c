/*---------------------------------------------------------------------------------

	deadstore.c -- passes over tcc's pseudo-registers that need liveness over
	the whole file's control flow (816-tcc fork):

	  deadStores       a store to tcc__rN whose value no path reads goes
	  forwardOperands  a value parked in tcc__rN for one use goes straight to
	                   that use:
	                     lda M / sta rN / lda ... / adc rN   ->  lda ... / adc M
	                       (adc sbc and ora eor cmp)
	                     sta rN / ... / ldx rN               ->  tax / ...
	                       (ldy: tay)
	                     stz rN / lda rN                     ->  lda.w #0

	816-opt's own rules look a few lines ahead in straight code; these follow
	branches, so a store before a loop's test, or one whose only reader was
	optimised away, goes too.

	The liveness rules, all on the side of keeping a store when unsure:
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

enum { K_NONE, K_LABEL, K_DIRECTIVE, K_INSN };

typedef struct {
    int kind;
    int labelled;           /* an anonymous label on the instruction's line */
    char mn[4];             /* lda, sta, ... */
    char *op;               /* the operand (a copy), "" if none */
    const char *rest;       /* the line after the mnemonic's 3 letters (.w #10, ...) */
    int m8, x8;             /* register widths here */
    int flow;               /* a branch, jump, call or return */
    regset use, def;
    int nsucc;
    int succ[2];
    int exit;               /* no successor: out is exitset */
    regset exitset;
    int store;              /* a whole store, deletable when dead */
    int keep;
} line_t;

typedef struct {
    size_t n;
    line_t *L;
    regset *in, *out;
    char **code;            /* each line without its comment */
} flow_t;

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
static char *code_of(const char *s)
{
    size_t i = 0, n = strlen(s);
    char *out = malloc(n + 1);
    if (!out)
        fatal("malloc-deadstore");
    while (s[i] && s[i] != ';') {
        out[i] = s[i];
        i++;
    }
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t'))
        i--;
    out[i] = 0;
    return out;
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

static int find_label(flow_t *F, const char *name)
{
    size_t i, k = strlen(name);
    for (i = 0; i < F->n; i++) {
        const char *c = F->code[i];
        if (!strncmp(c, name, k) && c[k] == ':' && c[k + 1] == 0)
            return (int) i;
    }
    return -1;
}

/* "+", "++" forward to the next line with that label; "-" backward */
static int find_anon(flow_t *F, int from, const char *ref)
{
    int n = (int) strlen(ref), i;
    char ch = ref[0];
    if (ch == '+') {
        for (i = from + 1; i < (int) F->n; i++)
            if (anon_label(F->code[i]) == n && F->code[i][0] == ch)
                return i;
    } else {
        for (i = from; i >= 0; i--)
            if (anon_label(F->code[i]) == n && F->code[i][0] == ch)
                return i;
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

static int is(const line_t *l, const char *mn) { return l->kind == K_INSN && !strcmp(l->mn, mn); }

/* every line's reads, writes and successors, then liveness */
static void analyze(dynArray *file, flow_t *F)
{
    size_t n = file->used, i;
    int m8 = 0, x8 = 0, changed;

    F->n = n;
    F->L = calloc(n ? n : 1, sizeof *F->L);
    F->in = calloc(n ? n : 1, sizeof *F->in);
    F->out = calloc(n ? n : 1, sizeof *F->out);
    F->code = calloc(n ? n : 1, sizeof *F->code);
    if (!F->L || !F->in || !F->out || !F->code)
        fatal("malloc-deadstore");
    for (i = 0; i < n; i++)
        F->code[i] = code_of(file->arr[i]);

    for (i = 0; i < n; i++) {
        line_t *l = &F->L[i];
        char *c = F->code[i], *op;
        int lab, k;

        l->keep = strstr(file->arr[i], "DON'T OPTIMIZE") != NULL;
        l->nsucc = 1;
        l->succ[0] = (int) i + 1;
        l->op = "";
        l->rest = "";
        l->m8 = m8;
        l->x8 = x8;

        if (c[0] == 0)
            continue;
        if (is_named_label(c)) {
            l->kind = K_LABEL;
            continue;
        }
        if (c[0] == '.') {
            l->kind = K_DIRECTIVE;
            if (!strncmp(c, ".ENDS", 5)) {      /* code doesn't run on into what follows */
                l->exit = 1;
                l->exitset = ALL;
                l->nsucc = 0;
            }
            continue;
        }
        lab = anon_label(c);
        if (lab) {
            l->kind = K_LABEL;
            l->labelled = 1;
            l->keep = 1;                        /* never drop a labelled line */
            c += lab;
            while (*c == ' ' || *c == '\t')
                c++;
            if (!*c)
                continue;
        }
        l->kind = K_INSN;

        /* the mnemonic's base (lda.b -> lda) and the operand */
        for (k = 0; k < 3 && c[k]; k++)
            l->mn[k] = (char) tolower((unsigned char) c[k]);
        l->mn[k] = 0;
        l->rest = c + k;
        op = strchr(c, ' ');
        while (op && (*op == ' ' || *op == '\t'))
            op++;
        l->op = op ? op : "";
        op = l->op;

        /* register widths, as the code runs down */
        if (is(l, "sep") || is(l, "rep")) {
            unsigned v = 0;
            const char *h = strchr(op, '$');
            if (h)
                v = (unsigned) strtoul(h + 1, NULL, 16);
            else if (*op == '#')
                v = (unsigned) strtoul(op + 1, NULL, 10);
            if (v & 0x20)
                m8 = (l->mn[0] == 's');
            if (v & 0x10)
                x8 = (l->mn[0] == 's');
        }

        if ((is(l, "sta") || is(l, "stz") || is(l, "stx") || is(l, "sty")) && strncmp(op, "tcc__", 5) == 0) {
            const char *e;
            int b = reg_bit(op, &e);
            int narrow = (l->mn[2] == 'a' || l->mn[2] == 'z') ? l->m8 : l->x8;
            if (b >= 0 && *e == 0 && !narrow) {
                l->def = (regset) 1 << b;
                l->store = 1;
                continue;
            }
        }

        l->use = regs_in(op);

        /* calls */
        if (is(l, "jsr") || is(l, "jsl")) {
            if (!strncmp(op, "tcc__", 5))
                l->use = ALL;
            l->def = ALL;
            l->flow = 1;
            continue;
        }

        /* returns */
        if (is(l, "rtl") || is(l, "rts") || is(l, "rti")) {
            const char *raw = file->arr[i];
            l->exit = 1;
            /* the compiler's mark says what the function returns
               (816-gen.c gfunc_epilog); without one, all it might */
            if (strstr(raw, "; retvoid"))
                l->exitset = 0;
            else if (strstr(raw, "; ret16"))
                l->exitset = (regset) 1 << 0;                           /* r0 */
            else if (strstr(raw, "; retptr"))
                l->exitset = ((regset) 1 << 0) | ((regset) 1 << 1);     /* r0, r0h */
            else if (strstr(raw, "; ret32"))
                l->exitset = ((regset) 1 << 0) | ((regset) 1 << 2);     /* r0, r1 */
            else
                l->exitset = ret_regs();
            l->nsucc = 0;
            l->flow = 1;
            continue;
        }
        if (is(l, "stp") || is(l, "wai") || is(l, "brk") || is(l, "cop")) {
            l->exit = 1;
            l->exitset = is(l, "stp") ? 0 : ALL;
            l->nsucc = 0;
            l->flow = 1;
            continue;
        }

        /* jumps and branches */
        {
            int cond = l->mn[0] == 'b' && !is(l, "bra") && !is(l, "brl") && !is(l, "bit");
            int uncond = is(l, "bra") || is(l, "brl") || is(l, "jmp") || is(l, "jml");
            if (cond || uncond) {
                int t = -1;
                l->flow = 1;
                if (strchr(op, '(') || strchr(op, '['))
                    t = -1;             /* indirect */
                else if (is_anon_ref(op))
                    t = find_anon(F, (int) i, op);
                else
                    t = find_label(F, op);
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
            line_t *l = &F->L[i];
            regset o = 0, ni;
            int s;
            if (l->exit)
                o = l->exitset;
            for (s = 0; s < l->nsucc; s++) {
                int t = l->succ[s];
                if (t >= (int) n)
                    o = ALL;            /* off the end */
                else
                    o |= F->in[t];
            }
            ni = l->use | (o & ~l->def);
            if (o != F->out[i] || ni != F->in[i]) {
                F->out[i] = o;
                F->in[i] = ni;
                changed = 1;
            }
        }
    } while (changed);
}

static void release(flow_t *F)
{
    size_t i;
    for (i = 0; i < F->n; i++)
        free(F->code[i]);
    free(F->code);
    free(F->L);
    free(F->in);
    free(F->out);
}

/* rebuild the file: drop the lines marked NULL in arr */
static void compact(dynArray *file)
{
    size_t i, w = 0;
    for (i = 0; i < file->used; i++)
        if (file->arr[i])
            file->arr[w++] = file->arr[i];
    file->used = w;
}

static void replace_line(dynArray *file, size_t i, const char *text)
{
    char *s = malloc(strlen(text) + 1);
    if (!s)
        fatal("malloc-deadstore");
    strcpy(s, text);
    free(file->arr[i]);
    file->arr[i] = s;
}

static void drop_line(dynArray *file, size_t i)
{
    free(file->arr[i]);
    file->arr[i] = NULL;
}

size_t deadStores(dynArray *file)
{
    flow_t F;
    size_t i, removed = 0;
    if (!file->used)
        return 0;
    analyze(file, &F);
    for (i = 0; i < F.n; i++)
        if (F.L[i].store && !F.L[i].keep && !(F.out[i] & F.L[i].def)) {
            drop_line(file, i);
            removed++;
        }
    release(&F);
    compact(file);
    return removed;
}

/* the register a whole store writes, or -1 */
static int stored_reg(const line_t *l)
{
    const char *e;
    int b;
    if (!l->store)
        return -1;
    b = reg_bit(l->op, &e);
    return b;
}

/* "<mn> tcc__X" exactly (ldx.b tcc__r0, adc.b tcc__r1): X's bit, or -1 */
static int reg_operand(const line_t *l, const char *mn)
{
    const char *e;
    int b;
    if (!is(l, mn))
        return -1;
    b = reg_bit(l->op, &e);
    return (b >= 0 && *e == 0) ? b : -1;
}

static int stack_op(const line_t *l)
{
    static const char *ops[] = {"pha", "phx", "phy", "php", "phb", "phd", "phk", "pea", "pei", "per",
                                "pla", "plx", "ply", "plp", "plb", "pld", "tcs", "tas", "txs", NULL};
    int k;
    for (k = 0; ops[k]; k++)
        if (is(l, ops[k]))
            return 1;
    return 0;
}

/* a store to memory, or a read-modify-write of it (anything that may
   change a memory operand) */
static int writes_memory(const line_t *l)
{
    static const char *st[] = {"sta", "stx", "sty", "stz", NULL};
    static const char *rmw[] = {"inc", "dec", "asl", "lsr", "rol", "ror", "tsb", "trb", NULL};
    int k;
    if (l->kind != K_INSN)
        return 0;
    if (!strncmp(l->op, "tcc__", 5) && reg_operand(l, l->mn) >= 0)
        return 0;               /* a pseudo-register: not the memory operand */
    for (k = 0; st[k]; k++)
        if (is(l, st[k]))
            return 1;
    for (k = 0; rmw[k]; k++)
        if (is(l, rmw[k]) && l->op[0] && strcmp(l->op, "a"))
            return 1;
    return is(l, "mvn") || is(l, "mvp");
}

/* reads or changes index register x ('x') or y ('y') */
static int touches_index(const line_t *l, char r)
{
    char a[8], b[8];
    static const char *xs[] = {"ldx", "inx", "dex", "tax", "txa", "txy", "tyx", "tsx", "txs", "phx", "plx", "stx", "cpx", NULL};
    static const char *ys[] = {"ldy", "iny", "dey", "tay", "tya", "txy", "tyx", "phy", "ply", "sty", "cpy", NULL};
    const char **t = (r == 'x') ? xs : ys;
    int k;
    if (l->kind != K_INSN)
        return 0;
    for (k = 0; t[k]; k++)
        if (is(l, t[k]))
            return 1;
    snprintf(a, sizeof a, ",%c", r);
    snprintf(b, sizeof b, ",%c", r - 32);
    if (strstr(l->op, a) || strstr(l->op, b))
        return 1;
    /* the block moves use x and y; mvn/mvp */
    return is(l, "mvn") || is(l, "mvp");
}

/* a plain operand an ALU instruction can take in place of the register:
   immediate, absolute, long, or stack-relative; no index, no indirection,
   no pseudo-register */
static int plain_operand(const line_t *l)
{
    const char *o = l->op;
    /* the pseudo-registers are out; tcc__a0.. (fastcall slots) are plain
       direct page memory, stores to them caught by writes_memory */
    if (!*o || regs_in(o) || strchr(o, '[') || strchr(o, '('))
        return 0;
    if (strstr(o, ",x") || strstr(o, ",X") || strstr(o, ",y") || strstr(o, ",Y"))
        return 0;
    return 1;
}

static int stack_relative(const line_t *l) { return strstr(l->op, ",s") || strstr(l->op, ",S"); }

/* A is written without being read (so an earlier load into A is dead) */
static int sets_a_fresh(const line_t *l)
{
    return (is(l, "lda") && !regs_in(l->op) ? 1 : 0) || is(l, "pla") || is(l, "tya") || is(l, "txa")
           || is(l, "tdc") || is(l, "tsc") || is(l, "tsa");
}

static int reads_flags(const line_t *l)
{
    int cond = l->kind == K_INSN && l->mn[0] == 'b' && !is(l, "bra") && !is(l, "brl") && !is(l, "bit");
    return cond || is(l, "php") || is(l, "adc") || is(l, "sbc") || is(l, "rol") || is(l, "ror");
}

/* A is written before it's read: a load, a pull or a transfer into it */
static int a_overwritten(const line_t *l)
{
    return is(l, "lda") || is(l, "pla") || is(l, "tya") || is(l, "txa") || is(l, "tdc") || is(l, "tsc")
           || is(l, "tsa");
}

/* A is dead after line i: the next instruction overwrites it or returns;
   an unconditional jump is followed to its target's first instruction */
static int a_dead_after(flow_t *F, size_t i)
{
    size_t j = i + 1;
    int hops;
    for (hops = 0; hops < 4; hops++) {
        line_t *l;
        while (j < F->n && (F->L[j].kind == K_NONE || (F->L[j].kind == K_LABEL && !F->L[j].labelled)))
            j++;
        if (j >= F->n)
            return 0;
        l = &F->L[j];
        if (l->kind != K_INSN)
            return 0;
        if (a_overwritten(l))
            return 1;
        if (is(l, "rtl") || is(l, "rts"))
            return 1;           /* the result goes back in tcc__r0, not A */
        if ((is(l, "bra") || is(l, "brl") || is(l, "jmp")) && l->nsucc == 1 && !l->exit) {
            j = (size_t) l->succ[0];
            continue;
        }
        return 0;
    }
    return 0;
}

#define WINDOW 16

size_t forwardOperands(dynArray *file)
{
    flow_t F;
    size_t i, done = 0;
    char *gone;
    char buf[MAXLEN_LINE];

    if (!file->used)
        return 0;
    analyze(file, &F);
    gone = calloc(F.n, 1);
    if (!gone)
        fatal("malloc-deadstore");

    for (i = 0; i + 1 < F.n; i++) {
        line_t *a = &F.L[i];
        size_t j;
        int r;

        if (gone[i] || a->kind != K_INSN || a->keep)
            continue;

        /* lda M / inc a / sta M  ->  inc M (dec too), M direct page or
           absolute, when A is dead after: the same N and Z, carry untouched */
        if (is(a, "lda") && !a->m8 && i + 2 < F.n && !gone[i + 1] && !gone[i + 2]
            && (!strncmp(a->rest, ".b ", 3) || !strncmp(a->rest, ".w ", 3)) && !strchr(a->op, ',')
            && !strchr(a->op, '#') && !strchr(a->op, '[') && !strchr(a->op, '(') && !regs_in(a->op)) {
            line_t *m = &F.L[i + 1], *s = &F.L[i + 2];
            int incdec = (is(m, "inc") || is(m, "dec")) && !strcmp(m->op, "a");
            if (incdec && !m->labelled && !m->keep && is(s, "sta") && !s->labelled && !s->keep && !s->m8
                && !strcmp(s->rest, a->rest) && a_dead_after(&F, i + 2)) {
                snprintf(buf, sizeof buf, "%s%s", m->mn, a->rest);
                replace_line(file, i, buf);
                drop_line(file, i + 1);
                drop_line(file, i + 2);
                gone[i] = gone[i + 1] = gone[i + 2] = 1;
                done++;
                continue;
            }
        }

        /* stz rN / lda rN  ->  lda.w #0 */
        r = stored_reg(a);
        if (r >= 0 && is(a, "stz") && !a->m8 && !gone[i + 1] && reg_operand(&F.L[i + 1], "lda") == r
            && !F.L[i + 1].keep && !F.L[i + 1].m8 && !(F.out[i + 1] & ((regset) 1 << r))) {
            replace_line(file, i + 1, "lda.w #0");
            drop_line(file, i);
            gone[i] = gone[i + 1] = 1;
            done++;
            continue;
        }

        /* lda M / sta rN / (A reloaded) ... / OP rN  ->  ... / OP M */
        if (is(a, "lda") && plain_operand(a) && !a->m8 && i + 2 < F.n) {
            line_t *s = &F.L[i + 1];
            int rs = stored_reg(s);
            if (rs >= 0 && is(s, "sta") && !s->keep && !gone[i + 1] && sets_a_fresh(&F.L[i + 2]) && !F.L[i + 2].labelled) {
                int sr = stack_relative(a);
                for (j = i + 2; j < F.n && j < i + 2 + WINDOW; j++) {
                    line_t *t = &F.L[j];
                    static const char *alu[] = {"adc", "sbc", "and", "ora", "eor", "cmp", NULL};
                    int k, hit = -1;
                    if (gone[j] || t->kind != K_INSN || t->labelled || t->keep || t->flow || t->m8)
                        break;
                    if (is(t, "sep") || is(t, "rep"))
                        break;
                    for (k = 0; alu[k]; k++)
                        if (reg_operand(t, alu[k]) == rs)
                            hit = k;
                    if (hit >= 0) {
                        if (!(F.out[j] & ((regset) 1 << rs))) {
                            snprintf(buf, sizeof buf, "%s%s", t->mn, a->rest);
                            replace_line(file, j, buf);
                            drop_line(file, i);
                            drop_line(file, i + 1);
                            gone[i] = gone[i + 1] = gone[j] = 1;
                            done++;
                        }
                        break;
                    }
                    if (t->use & ((regset) 1 << rs) || t->def & ((regset) 1 << rs))
                        break;
                    if (writes_memory(t))
                        break;
                    if (sr && stack_op(t))
                        break;
                }
                if (gone[i])
                    continue;
            }
        }

        /* sta rN / ... / ldx rN  ->  tax / ...   (ldy: tay) */
        r = stored_reg(a);
        if (r >= 0 && is(a, "sta") && !a->m8 && !a->x8) {
            for (j = i + 1; j < F.n && j < i + 1 + WINDOW; j++) {
                line_t *t = &F.L[j];
                char ix = 0;
                if (gone[j] || t->kind != K_INSN || t->labelled || t->keep || t->flow)
                    break;
                if (is(t, "sep") || is(t, "rep"))
                    break;
                if (reg_operand(t, "ldx") == r)
                    ix = 'x';
                else if (reg_operand(t, "ldy") == r)
                    ix = 'y';
                if (ix) {
                    /* the line after mustn't test the flags ldx set */
                    int after_ok = j + 1 < F.n && !reads_flags(&F.L[j + 1]);
                    if (after_ok && !(F.out[j] & ((regset) 1 << r)) && !t->x8) {
                        int k, clean = 1;
                        for (k = (int) i + 1; k < (int) j; k++)
                            if (touches_index(&F.L[k], ix) || reads_flags(&F.L[k]))
                                clean = 0;
                        /* tax sets N and Z where sta didn't: nothing between
                           may read them (reads_flags above), and adc/sbc's
                           carry is untouched by tax */
                        if (clean) {
                            replace_line(file, i, ix == 'x' ? "tax" : "tay");
                            drop_line(file, j);
                            gone[i] = gone[j] = 1;
                            done++;
                        }
                    }
                    break;
                }
                if (t->use & ((regset) 1 << r) || t->def & ((regset) 1 << r))
                    break;
            }
        }
    }

    free(gone);
    release(&F);
    compact(file);
    return done;
}
