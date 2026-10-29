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
    int vol;                /* "; volatile": an access 816-tcc marked, never dropped, moved or reused */
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
        l->vol = strstr(file->arr[i], "; volatile") != NULL;
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
/* the dropped lines out, and a replacement of several lines ("tya\nasl a")
   split into its lines, so the next pass sees each */
static void compact(dynArray *file)
{
    size_t i, w = 0, extra = 0;
    for (i = 0; i < file->used; i++)
        if (file->arr[i])
            file->arr[w++] = file->arr[i];
    file->used = w;
    for (i = 0; i < file->used; i++) {
        const char *p;
        for (p = file->arr[i]; (p = strchr(p, '\n')) != NULL; p++)
            extra++;
    }
    if (extra) {
        char **out = malloc((file->used + extra) * sizeof *out);
        size_t o = 0;
        if (!out)
            fatal("malloc-deadstore");
        for (i = 0; i < file->used; i++) {
            char *s = file->arr[i], *nl;
            while ((nl = strchr(s, '\n')) != NULL) {
                char *piece = malloc(nl - s + 1);
                if (!piece)
                    fatal("malloc-deadstore");
                memcpy(piece, s, nl - s);
                piece[nl - s] = 0;
                out[o++] = piece;
                s = nl + 1;
            }
            {
                char *last = malloc(strlen(s) + 1);
                if (!last)
                    fatal("malloc-deadstore");
                strcpy(last, s);
                out[o++] = last;
            }
            free(file->arr[i]);
        }
        free(file->arr);
        file->arr = out;
        file->used = o;
    }
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

static int is_cond_branch(const line_t *l);
static int mentions_reg(const char *s, int bit);
static int nz_needed_after(flow_t *F, size_t i);

/* the line names direct page slot "tcc__lK" (the whole word) */
static int mentions_dp(const char *s, const char *name)
{
    size_t n = strlen(name);
    const char *p = s;
    while ((p = strstr(p, name)) != NULL) {
        if (!isalnum((unsigned char) p[n]) && p[n] != '_')
            return 1;
        p += n;
    }
    return 0;
}

/* A is dead from line j on: every path loads it anew (or returns, or calls:
   a callee doesn't read the caller's A) before anything reads it; past
   instructions that don't touch A, through jumps, both ways at a branch */
static int a_dead_from(flow_t *F, size_t j, int depth)
{
    static const char *noa[] = {"ldx", "ldy", "stx", "sty", "stz", "inx", "dex", "iny", "dey", "cpx", "cpy", "txy",
                                "tyx", "pea", "pei", "phx", "phy", "plx", "ply", "clc", "sec", "cli", "sei", "nop",
                                "rep", "sep", NULL};
    int steps = 0;
    if (depth > 6)
        return 0;
    while (j < F->n && steps++ < 64) {
        line_t *l = &F->L[j];
        int k, quiet = 0;
        if (l->kind == K_NONE || l->kind == K_LABEL) {
            j++;
            continue;
        }
        if (l->kind != K_INSN)
            return 0;           /* a directive: conditional code */
        if (a_overwritten(l) || is(l, "rtl") || is(l, "rts") || is(l, "jsr") || is(l, "jsl"))
            return 1;
        if ((is(l, "bra") || is(l, "brl") || is(l, "jmp")) && l->nsucc == 1 && !l->exit) {
            j = (size_t) l->succ[0];
            if (++depth > 6)
                return 0;
            continue;
        }
        if (is_cond_branch(l)) {
            if (l->nsucc != 2 || !a_dead_from(F, (size_t) l->succ[1], depth + 1))
                return 0;
            j++;
            continue;
        }
        for (k = 0; noa[k]; k++)
            if (is(l, noa[k]))
                quiet = 1;
        if ((is(l, "asl") || is(l, "lsr") || is(l, "rol") || is(l, "ror") || is(l, "inc") || is(l, "dec")) && l->op[0]
            && strcmp(l->op, "a"))
            quiet = 1;          /* on memory: A untouched */
        if (!quiet || l->flow)
            return 0;
        j++;
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

        /* a load nothing reads: lda of a pseudo-register or slot (no side
           effect, unlike an I/O register) with A dead after and its N and Z
           unread */
        if (is(a, "lda") && !strncmp(a->op, "tcc__", 5) && !a->vol && !a->labelled && !strchr(a->op, ',')
            && a_dead_from(&F, i + 1, 0) && !nz_needed_after(&F, i)) {
            drop_line(file, i);
            gone[i] = 1;
            done++;
            continue;
        }

        /* a long pointer copied from direct page slots (816-gen.c: a pointer
           local in tcc__lK and lK+1) and read through:
             lda.b tcc__lK / sta.b rN / lda.b tcc__lK+1 / sta.b rNh
             ... [rN] ...           ->  ... [tcc__lK] ...
           while neither pair changes, with no label or branch between; the
           copy, unread then, goes as a dead store */
        if (is(a, "lda") && !strncmp(a->op, "tcc__l", 6) && isdigit((unsigned char) a->op[6]) && !a->op[7]
            && !a->m8 && i + 3 < F.n && !gone[i + 1] && !gone[i + 2] && !gone[i + 3]) {
            int k = a->op[6] - '0', rn = stored_reg(&F.L[i + 1]);
            line_t *c2 = &F.L[i + 2];
            char lo[16], hi[16], ind[24], to[24];
            snprintf(lo, sizeof lo, "tcc__l%d", k);
            snprintf(hi, sizeof hi, "tcc__l%d", k + 1);
            if (rn >= 0 && rn < 32 && !(rn & 1) && is(&F.L[i + 1], "sta") && is(c2, "lda") && !strcmp(c2->op, hi)
                && stored_reg(&F.L[i + 3]) == rn + 1 && is(&F.L[i + 3], "sta") && !F.L[i + 1].labelled
                && !c2->labelled && !F.L[i + 3].labelled) {
                size_t u;
                int hits = 0;
                snprintf(ind, sizeof ind, "[tcc__r%d]", rn / 2);
                snprintf(to, sizeof to, "[tcc__l%d]", k);
                for (u = i + 4; u < F.n; u++) {
                    line_t *m = &F.L[u];
                    char *at;
                    if (gone[u] || m->kind != K_INSN || m->labelled || m->flow)
                        break;
                    if (mentions_dp(F.code[u], lo) || mentions_dp(F.code[u], hi))
                        break;          /* the slot read or changed: stop here */
                    if ((at = strstr(F.code[u], ind)) != NULL && !m->keep) {
                        snprintf(buf, sizeof buf, "%.*s%s%s", (int) (at - F.code[u]), F.code[u], to,
                                 at + strlen(ind));
                        replace_line(file, u, buf);
                        gone[u] = 1;
                        hits++;
                    }
                    if (stored_reg(m) == rn || stored_reg(m) == rn + 1 || ((m->def >> rn) & 1)
                        || ((m->def >> (rn + 1)) & 1))
                        break;
                    if (!((F.out[u] >> rn) & 1) && !((F.out[u] >> (rn + 1)) & 1))
                        break;          /* the copy is dead from here */
                }
                if (hits) {
                    done++;
                    continue;
                }
            }
        }

        /* an unsigned byte loaded:
             lda.w #0 / sep #$20 / lda M / rep #$20  ->  lda M / and.w #$00ff
           (8 cycles, not 13). It reads the byte after M too, so M is a named
           object (a symbol or a pseudo-register, not an address or through
           a pointer) and not volatile; the next instruction doesn't look at
           N (bit 15 now, bit 7 before) */
        if (is(a, "lda") && !a->m8 && !strcmp(a->rest, ".w #0") && i + 4 < F.n && !gone[i + 1] && !gone[i + 2]
            && !gone[i + 3] && is(&F.L[i + 1], "sep") && !strcmp(F.L[i + 1].op, "#$20") && is(&F.L[i + 2], "lda")
            && is(&F.L[i + 3], "rep") && !strcmp(F.L[i + 3].op, "#$20") && !F.L[i + 1].labelled
            && !F.L[i + 2].labelled && !F.L[i + 3].labelled && !F.L[i + 2].vol && !F.L[i + 2].keep
            && (isalpha((unsigned char) F.L[i + 2].op[0]) || F.L[i + 2].op[0] == '_')
            && !strchr(F.L[i + 2].op, '[') && !strchr(F.L[i + 2].op, '(')) {
            static const char *next_ok[] = {"sta", "xba", "clc", "sec", "cmp", "and", "ora", "eor", "adc", "sbc",
                                            "tax", "tay", "pha", NULL};
            line_t *nx = &F.L[i + 4];
            int k, fine = 0;
            for (k = 0; next_ok[k]; k++)
                if (is(nx, next_ok[k]))
                    fine = 1;
            if (fine && !nx->labelled) {
                snprintf(buf, sizeof buf, "lda%s", F.L[i + 2].rest);
                replace_line(file, i, buf);
                replace_line(file, i + 1, "and.w #$00ff");
                drop_line(file, i + 2);
                drop_line(file, i + 3);
                gone[i] = gone[i + 1] = gone[i + 2] = gone[i + 3] = 1;
                done++;
                continue;
            }
        }

        /* the value parked to be added to something loaded next:
             sta.b rN / lda M / [clc] / adc.b rN  ->  [clc] / adc M
           (and, ora, eor too: all commutative), rN dead after */
        r = stored_reg(a);
        if (r >= 0 && r < 32 && !(r & 1) && is(a, "sta") && !a->m8 && i + 2 < F.n && !gone[i + 1]
            && is(&F.L[i + 1], "lda") && !F.L[i + 1].labelled && !F.L[i + 1].vol && !F.L[i + 1].keep
            && !F.L[i + 1].m8 && F.L[i + 1].op[0] && !mentions_reg(F.code[i + 1], r)
            && !mentions_reg(F.code[i + 1], r + 1) && reg_operand(&F.L[i + 1], "lda") != r) {
            static const char *comm[] = {"adc", "and", "ora", "eor", NULL};
            size_t u = i + 2;
            int k, hit = -1;
            if (is(&F.L[u], "clc") && !F.L[u].labelled && u + 1 < F.n)
                u++;
            for (k = 0; comm[k]; k++)
                if (reg_operand(&F.L[u], comm[k]) == r && (k == 0 || u == i + 2))
                    hit = k;    /* adc after its clc; the others straight after */
            if (hit >= 0 && !gone[u] && !F.L[u].labelled && !F.L[u].m8 && !((F.out[u] >> r) & 1)
                && !((F.out[u] >> (r + 1)) & 1)) {
                snprintf(buf, sizeof buf, "%s%s", comm[hit], F.L[i + 1].rest);
                replace_line(file, u, buf);
                drop_line(file, i);
                drop_line(file, i + 1);
                gone[i] = gone[i + 1] = gone[u] = 1;
                done++;
                continue;
            }
        }

        /* a loop's counter in Y (816-gen.c yl_*):
             sty rN / lda rN        ->  tya          (rN dead after)
             tya / cmp X            ->  cpy X        (A dead after; X immediate,
                                                      direct page or absolute)
             tya / inc a / tay      ->  iny          (dec: dey; A dead after) */
        r = stored_reg(a);
        if (r >= 0 && is(a, "sty") && !a->x8 && !gone[i + 1] && reg_operand(&F.L[i + 1], "lda") == r
            && !F.L[i + 1].keep && !F.L[i + 1].labelled && !F.L[i + 1].m8 && !(F.out[i + 1] & ((regset) 1 << r))) {
            replace_line(file, i, "tya");
            drop_line(file, i + 1);
            gone[i] = gone[i + 1] = 1;
            done++;
            continue;
        }
        if (r >= 0 && is(a, "sty") && !a->x8 && i + 2 < F.n && !gone[i + 1] && !gone[i + 2]
            && (reg_operand(&F.L[i + 1], "inc") == r || reg_operand(&F.L[i + 1], "dec") == r)
            && reg_operand(&F.L[i + 2], "ldy") == r && !F.L[i + 1].labelled && !F.L[i + 2].labelled
            && !F.L[i + 2].keep && !(F.out[i + 2] & ((regset) 1 << r))) {
            /* sty rN / inc rN / ldy rN  ->  iny (dec: dey) */
            replace_line(file, i, is(&F.L[i + 1], "inc") ? "iny" : "dey");
            drop_line(file, i + 1);
            drop_line(file, i + 2);
            gone[i] = gone[i + 1] = gone[i + 2] = 1;
            done++;
            continue;
        }
        /* sty rN / asl rN  ->  tya / asl a / sta rN (stx: txa; lsr rol ror
           inc dec too), and sta rN / asl rN  ->  asl a / sta rN, when A is
           dead after: a shift in A, and the value parked where tax / tay can
           take it */
        if (r >= 0 && (is(a, "sty") || is(a, "stx") || is(a, "sta")) && !a->x8 && !a->m8 && i + 1 < F.n
            && !gone[i + 1]) {
            line_t *m = &F.L[i + 1];
            static const char *rmw[] = {"asl", "lsr", "rol", "ror", "inc", "dec", NULL};
            int k, hit = 0;
            for (k = 0; rmw[k]; k++)
                if (reg_operand(m, rmw[k]) == r)
                    hit = 1;
            if (hit && !m->labelled && !m->keep && a_dead_from(&F, i + 2, 0)) {
                char b2[32], b3[32];
                snprintf(b2, sizeof b2, "%s a", m->mn);
                snprintf(b3, sizeof b3, "sta.b tcc__r%d", r / 2);
                if (is(a, "sta")) {
                    replace_line(file, i, b2);
                    replace_line(file, i + 1, b3);
                } else {
                    char b1[64];
                    snprintf(b1, sizeof b1, "%s\n%s", is(a, "sty") ? "tya" : "txa", b2);
                    replace_line(file, i, b1);
                    replace_line(file, i + 1, b3);
                }
                gone[i] = gone[i + 1] = 1;
                done++;
                continue;
            }
        }
        if (is(a, "tya") && !a->m8 && !a->x8 && i + 1 < F.n && !gone[i + 1]) {
            line_t *m = &F.L[i + 1];
            if (is(m, "cmp") && !m->labelled && !m->keep && !m->vol && m->op[0] && !strchr(m->op, ',')
                && !strchr(m->op, '[') && !strchr(m->op, '(') && strncmp(m->rest, ".l", 2) && a_dead_from(&F, i + 2, 0)) {
                snprintf(buf, sizeof buf, "cpy%s", m->rest);
                replace_line(file, i + 1, buf);
                drop_line(file, i);
                gone[i] = gone[i + 1] = 1;
                done++;
                continue;
            }
            if (i + 2 < F.n && !gone[i + 2] && (is(m, "inc") || is(m, "dec")) && !strcmp(m->op, "a") && !m->labelled
                && is(&F.L[i + 2], "tay") && !F.L[i + 2].labelled && a_dead_from(&F, i + 3, 0)) {
                replace_line(file, i, is(m, "inc") ? "iny" : "dey");
                drop_line(file, i + 1);
                drop_line(file, i + 2);
                gone[i] = gone[i + 1] = gone[i + 2] = 1;
                done++;
                continue;
            }
        }

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
        if (is(a, "lda") && plain_operand(a) && !a->m8 && !a->vol && i + 2 < F.n) {
            line_t *s = &F.L[i + 1];
            int rs = stored_reg(s);
            size_t f = i + 2;
            while (f < F.n && (is(&F.L[f], "clc") || is(&F.L[f], "sec")) && !F.L[f].labelled)
                f++;            /* the carry set up for the use: A untouched */
            if (rs >= 0 && is(s, "sta") && !s->keep && !gone[i + 1] && f < F.n && sets_a_fresh(&F.L[f])
                && !F.L[f].labelled) {
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

/* ============================================================
   knownRegs: what A, X and Y hold, as "the value of operand M", by forward
   dataflow; a load of what a register already holds goes.
   ============================================================ */

#define K_TOP (-2)              /* not reached yet */
#define K_UNK (-1)

typedef struct {
    int a, x, y;
} regs_t;

typedef struct {
    char **keys;
    int n, cap;
} keytab_t;

static int key_of(keytab_t *T, const char *s)
{
    int i;
    for (i = 0; i < T->n; i++)
        if (!strcmp(T->keys[i], s))
            return i;
    if (T->n == T->cap) {
        T->cap = T->cap ? T->cap * 2 : 64;
        T->keys = realloc(T->keys, T->cap * sizeof *T->keys);
        if (!T->keys)
            fatal("malloc-deadstore");
    }
    T->keys[T->n] = malloc(strlen(s) + 1);
    if (!T->keys[T->n])
        fatal("malloc-deadstore");
    strcpy(T->keys[T->n], s);
    return T->n++;
}

/* an operand whose value a register can be said to hold: immediate,
   direct page, absolute, long or stack-relative; no index, no indirection */
static int trackable(const line_t *l)
{
    const char *o = l->op;
    if (!*o || strchr(o, '[') || strchr(o, '('))
        return 0;
    if (strstr(o, ",x") || strstr(o, ",X") || strstr(o, ",y") || strstr(o, ",Y"))
        return 0;
    return 1;
}

static int key_is_memory(keytab_t *T, int k) { return k >= 0 && !strchr(T->keys[k], '#'); }
static int key_is_stack(keytab_t *T, int k) { return k >= 0 && (strstr(T->keys[k], ",s") || strstr(T->keys[k], ",S")); }

static void forget_memory(keytab_t *T, regs_t *r)
{
    if (key_is_memory(T, r->a))
        r->a = K_UNK;
    if (key_is_memory(T, r->x))
        r->x = K_UNK;
    if (key_is_memory(T, r->y))
        r->y = K_UNK;
}

static void forget_stack(keytab_t *T, regs_t *r)
{
    if (key_is_stack(T, r->a))
        r->a = K_UNK;
    if (key_is_stack(T, r->x))
        r->x = K_UNK;
    if (key_is_stack(T, r->y))
        r->y = K_UNK;
}

static int meet1(int a, int b)
{
    if (a == K_TOP)
        return b;
    if (b == K_TOP)
        return a;
    return a == b ? a : K_UNK;
}

static int is_cond_branch(const line_t *l)
{
    return l->kind == K_INSN && l->mn[0] == 'b' && !is(l, "bra") && !is(l, "brl") && !is(l, "bit");
}

/* the registers after line l, from those before */
static void transfer(keytab_t *T, const line_t *l, regs_t *r)
{
    static const char *nochange[] = {"clc", "sec", "cld", "sed", "cli", "sei", "clv", "nop", "cmp", "cpx", "cpy",
                                     "bit", "bra", "brl", "jmp", "jml", "wdm", NULL};
    static const char *stackops[] = {"pha", "phx", "phy", "php", "phb", "phd", "phk", "pea", "pei", "per", "plp",
                                     "plb", "pld", NULL};
    int k;
    if (l->kind == K_DIRECTIVE) {
        r->a = r->x = r->y = K_UNK;     /* conditional assembly: maybe not there */
        return;
    }
    if (l->kind != K_INSN)
        return;
    if (is(l, "lda") || is(l, "ldx") || is(l, "ldy")) {
        int v = (trackable(l) && !l->vol) ? key_of(T, l->rest) : K_UNK;
        if (is(l, "lda"))
            r->a = v;
        else if (is(l, "ldx"))
            r->x = v;
        else
            r->y = v;
        return;
    }
    if (is(l, "sta") || is(l, "stx") || is(l, "sty") || is(l, "stz")) {
        forget_memory(T, r);
        if (trackable(l) && !strchr(l->op, '#') && !l->vol) {
            int v = key_of(T, l->rest);
            if (is(l, "sta"))
                r->a = v;
            else if (is(l, "stx"))
                r->x = v;
            else if (is(l, "sty"))
                r->y = v;
        }
        return;
    }
    if (is(l, "tax")) { r->x = r->a; return; }
    if (is(l, "tay")) { r->y = r->a; return; }
    if (is(l, "txa")) { r->a = r->x; return; }
    if (is(l, "tya")) { r->a = r->y; return; }
    if (is(l, "txy")) { r->y = r->x; return; }
    if (is(l, "tyx")) { r->x = r->y; return; }
    if (is(l, "inx") || is(l, "dex") || is(l, "tsx") || is(l, "plx")) { r->x = K_UNK; forget_stack(T, r); return; }
    if (is(l, "iny") || is(l, "dey") || is(l, "ply")) { r->y = K_UNK; forget_stack(T, r); return; }
    if (is(l, "pla")) { r->a = K_UNK; forget_stack(T, r); return; }
    for (k = 0; stackops[k]; k++)
        if (is(l, stackops[k])) {
            forget_stack(T, r);
            return;
        }
    if (is(l, "tcs") || is(l, "tas") || is(l, "txs")) { forget_stack(T, r); return; }
    for (k = 0; nochange[k]; k++)
        if (is(l, nochange[k]))
            return;
    if (is_cond_branch(l))
        return;
    /* read-modify-write on memory */
    if ((is(l, "inc") || is(l, "dec") || is(l, "asl") || is(l, "lsr") || is(l, "rol") || is(l, "ror")) && l->op[0]
        && strcmp(l->op, "a")) {
        forget_memory(T, r);
        return;
    }
    if (is(l, "tsb") || is(l, "trb")) {
        forget_memory(T, r);
        return;
    }
    /* A changed: arithmetic, shifts and the like on A */
    if (is(l, "adc") || is(l, "sbc") || is(l, "and") || is(l, "ora") || is(l, "eor") || is(l, "xba") || is(l, "inc")
        || is(l, "dec") || is(l, "asl") || is(l, "lsr") || is(l, "rol") || is(l, "ror") || is(l, "tdc")
        || is(l, "tsc") || is(l, "tsa")) {
        r->a = K_UNK;
        return;
    }
    /* calls, tcd, sep/rep, block moves, anything else: nothing known */
    r->a = r->x = r->y = K_UNK;
}

/* N and Z as a load left them are read before anything sets them again */
static int nz_needed_after(flow_t *F, size_t i)
{
    size_t j;
    static const char *writers[] = {"lda", "ldx", "ldy", "adc", "sbc", "and", "ora", "eor", "cmp", "cpx", "cpy", "bit",
                                    "inc", "dec", "inx", "dex", "iny", "dey", "asl", "lsr", "rol", "ror", "tax",
                                    "tay", "txa", "tya", "tsx", "txy", "tyx", "pla", "plx", "ply", "plp", "tdc",
                                    "tsc", "tsa", "xba", NULL};
    for (j = i + 1; j < F->n && j < i + 32; j++) {
        line_t *l = &F->L[j];
        int k;
        if (l->kind == K_NONE)
            continue;
        if (l->kind != K_INSN || l->labelled)
            return 1;           /* a label or directive: someone may test them */
        if (is_cond_branch(l) || is(l, "php"))
            return 1;
        for (k = 0; writers[k]; k++)
            if (is(l, writers[k]))
                return 0;
        if (is(l, "rtl") || is(l, "rts") || is(l, "jsr") || is(l, "jsl"))
            return 0;           /* nobody reads flags across a call or a return */
        if (l->flow)
            return 1;
    }
    return 1;
}

size_t knownRegs(dynArray *file)
{
    flow_t F;
    keytab_t T = {0};
    regs_t *in, *out;
    int **preds, *npred, changed;
    size_t i, done = 0;

    if (!file->used)
        return 0;
    analyze(file, &F);
    in = malloc(F.n * sizeof *in);
    out = malloc(F.n * sizeof *out);
    preds = calloc(F.n, sizeof *preds);
    npred = calloc(F.n, sizeof *npred);
    if (!in || !out || !preds || !npred)
        fatal("malloc-deadstore");

    /* predecessors */
    for (i = 0; i < F.n; i++) {
        int s;
        for (s = 0; s < F.L[i].nsucc; s++) {
            int t = F.L[i].succ[s];
            if (t >= 0 && t < (int) F.n)
                npred[t]++;
        }
    }
    for (i = 0; i < F.n; i++) {
        preds[i] = malloc((npred[i] ? npred[i] : 1) * sizeof **preds);
        if (!preds[i])
            fatal("malloc-deadstore");
        npred[i] = 0;
    }
    for (i = 0; i < F.n; i++) {
        int s;
        for (s = 0; s < F.L[i].nsucc; s++) {
            int t = F.L[i].succ[s];
            if (t >= 0 && t < (int) F.n)
                preds[t][npred[t]++] = (int) i;
        }
    }

    for (i = 0; i < F.n; i++)
        in[i].a = in[i].x = in[i].y = out[i].a = out[i].x = out[i].y = K_TOP;
    do {
        changed = 0;
        for (i = 0; i < F.n; i++) {
            regs_t r;
            int p, ours;
            /* a named label is an entry from anywhere (a function), unless
               it's one of the compiler's own: __local_ and __c labels are
               reached only by the jumps in this file, which agree or not */
            ours = F.L[i].kind == K_LABEL && !F.L[i].labelled
                   && (!strncmp(F.code[i], "__local_", 8) || !strncmp(F.code[i], "__c", 3));
            if (npred[i] == 0 || (F.L[i].kind == K_LABEL && !ours))
                r.a = r.x = r.y = K_UNK;
            else {
                r.a = r.x = r.y = K_TOP;
                for (p = 0; p < npred[i]; p++) {
                    regs_t *q = &out[preds[i][p]];
                    r.a = meet1(r.a, q->a);
                    r.x = meet1(r.x, q->x);
                    r.y = meet1(r.y, q->y);
                }
            }
            if (r.a != in[i].a || r.x != in[i].x || r.y != in[i].y) {
                in[i] = r;
                changed = 1;
            }
            transfer(&T, &F.L[i], &r);
            if (r.a != out[i].a || r.x != out[i].x || r.y != out[i].y) {
                out[i] = r;
                changed = 1;
            }
        }
    } while (changed);

    for (i = 0; i < F.n; i++) {
        line_t *l = &F.L[i];
        int have, kx, ka, ky;
        if (l->kind != K_INSN || l->keep || l->labelled || l->vol || !trackable(l))
            continue;
        if (is(l, "lda"))
            have = in[i].a;
        else if (is(l, "ldx"))
            have = in[i].x;
        else if (is(l, "ldy"))
            have = in[i].y;
        else
            continue;
        if (have >= 0 && !strcmp(T.keys[have], l->rest) && !nz_needed_after(&F, i)) {
            drop_line(file, i);
            done++;
            continue;
        }
        /* another register holds it: a transfer (2 cycles, the same N and Z),
           with both registers 16 bits */
        if (l->m8 || l->x8)
            continue;
        ka = in[i].a >= 0 && !strcmp(T.keys[in[i].a], l->rest);
        kx = in[i].x >= 0 && !strcmp(T.keys[in[i].x], l->rest);
        ky = in[i].y >= 0 && !strcmp(T.keys[in[i].y], l->rest);
        {
            const char *t = NULL;
            if (is(l, "lda"))
                t = kx ? "txa" : ky ? "tya" : NULL;
            else if (is(l, "ldx"))
                t = ka ? "tax" : ky ? "tyx" : NULL;
            else if (is(l, "ldy"))
                t = ka ? "tay" : kx ? "txy" : NULL;
            if (t) {
                replace_line(file, i, t);
                done++;
                continue;
            }
        }
        /* lda.b rA / adc.b rB (and ora eor, all commutative) with rB in X or Y
           and both dead after: the other way round, so the load can become a
           transfer next time */
        if (is(l, "lda") && i + 1 < F.n && !l->labelled && reg_operand(l, "lda") >= 0) {
            line_t *m = &F.L[i + 1];
            int ra = reg_operand(l, "lda"), rb = -1;
            static const char *comm[] = {"adc", "and", "ora", "eor", NULL};
            int k;
            for (k = 0; comm[k]; k++)
                if (reg_operand(m, comm[k]) >= 0)
                    rb = reg_operand(m, comm[k]);
            if (rb >= 0 && rb != ra && !m->labelled && !m->keep && !m->m8
                && !((F.out[i + 1] >> ra) & 1) && !((F.out[i + 1] >> rb) & 1)) {
                char want[64];
                snprintf(want, sizeof want, ".b tcc__r%d", rb / 2);
                if ((rb & 1) == 0 && (ra & 1) == 0
                    && ((in[i].x >= 0 && !strcmp(T.keys[in[i].x], want))
                        || (in[i].y >= 0 && !strcmp(T.keys[in[i].y], want)))) {
                    char b1[64], b2[64];
                    snprintf(b1, sizeof b1, "lda.b tcc__r%d", rb / 2);
                    snprintf(b2, sizeof b2, "%s.b tcc__r%d", m->mn, ra / 2);
                    replace_line(file, i, b1);
                    replace_line(file, i + 1, b2);
                    done++;
                    i++;
                    continue;
                }
            }
        }
    }

    for (i = 0; i < F.n; i++)
        free(preds[i]);
    free(preds);
    free(npred);
    free(in);
    free(out);
    for (i = 0; i < (size_t) T.n; i++)
        free(T.keys[i]);
    free(T.keys);
    release(&F);
    compact(file);
    return done;
}

/* ============================================================
   reuseBlocks: a computation done again, from the same sources, into
   another pseudo-register goes; its uses read the first one's register.

     lda.b l0 / sta.b r0 / asl a / asl a / clc / adc.b r0 / asl a / sta.b r0
     lda.b l0 / sta.b r1 / asl a / asl a / clc / adc.b r1 / asl a / sta.b r1
     ldx.b r0 ... ldx.b r1 ...        ->  the second block goes, ldx.b r0

   A block: from an lda through the last "sta.b tcc__rK" before A is next
   loaded; inside, only A's own operations, reads of memory and the
   register's own stores (rK). Conditions: the blocks match line for line
   with rA for rB; between them nothing stores to memory the first reads,
   to rA, moves the stack (for a stack operand), branches, or has a label;
   after the second, A and the flags it left are not needed, rB is dead
   before any branch or label, and rA is not written while rB's uses are
   renamed.
   ============================================================ */

/* the line with every "tcc__<from>" (whole token) as "tcc__<to>" */
static void rename_reg(const char *s, int from, int to, char *out, size_t n)
{
    char fname[16], tname[16];
    size_t fl, o = 0;
    snprintf(fname, sizeof fname, "tcc__r%d", from / 2);
    snprintf(tname, sizeof tname, "tcc__r%d", to / 2);
    fl = strlen(fname);
    while (*s && o + 16 < n) {
        if (!strncmp(s, fname, fl)) {
            char c = s[fl];
            if (!((c >= '0' && c <= '9') || c == 'h' || (c >= 'a' && c <= 'z') || c == '_')) {
                size_t k;
                for (k = 0; tname[k]; k++)
                    out[o++] = tname[k];
                s += fl;
                continue;
            }
        }
        out[o++] = *s++;
    }
    out[o] = 0;
}

static int mentions_reg(const char *s, int bit)
{
    return (regs_in(s) >> bit) & 1;
}

/* A's own operations, a block may contain */
static int a_only(const line_t *l)
{
    static const char *ok[] = {"asl", "lsr", "rol", "ror", "inc", "dec", "adc", "sbc", "and", "ora", "eor", "clc",
                               "sec", "xba", "cmp", NULL};
    int k;
    for (k = 0; ok[k]; k++)
        if (is(l, ok[k])) {
            /* shifts and inc/dec on A only, not on memory */
            if ((is(l, "asl") || is(l, "lsr") || is(l, "rol") || is(l, "ror") || is(l, "inc") || is(l, "dec"))
                && l->op[0] && strcmp(l->op, "a"))
                return 0;
            return 1;
        }
    return 0;
}

/* what the last block_at began from: 0 memory, 'x' or 'y' (a loop's
   counter in Y: sty.b rK / tya, the register's value parked in rK too) */
static int block_src;

/* the block starting at s (an lda, or sty.b rK / tya, stx.b rK / txa): its
   end, and the register it makes, or -1 */
static int block_at(flow_t *F, size_t s, size_t *end, int *reg)
{
    size_t j, last = 0, first = s + 1;
    int r = -1;
    line_t *l = &F->L[s];
    block_src = 0;
    if ((is(l, "sty") || is(l, "stx")) && !l->keep && !l->labelled && !l->x8 && !l->m8 && s + 1 < F->n) {
        int b = stored_reg(l);
        line_t *t = &F->L[s + 1];
        if (b < 0 || b >= 32 || (b & 1) || t->labelled || !is(t, is(l, "sty") ? "tya" : "txa"))
            return -1;
        r = b;
        last = s;
        first = s + 2;
        block_src = is(l, "sty") ? 'y' : 'x';
    } else if (!is(l, "lda") || l->keep || l->labelled || l->m8 || l->vol || !plain_operand(l))
        return -1;
    for (j = first; j < F->n && j < s + 24; j++) {
        line_t *t = &F->L[j];
        int b;
        if (t->kind != K_INSN || t->labelled || t->keep || t->flow || t->m8 || t->vol)
            break;
        b = stored_reg(t);
        if (b >= 0 && b < 32 && is(t, "sta") && !(b & 1)) {
            if (r < 0)
                r = b;
            if (b != r)
                break;
            last = j;
            continue;
        }
        if (a_only(t) && !strchr(t->op, ',') && !strchr(t->op, '[') && !strchr(t->op, '('))
            continue;
        break;
    }
    if (r < 0 || last <= s + (block_src ? 1 : 0))
        return -1;
    *end = last;
    *reg = r;
    return 0;
}

/* flags as the dropped block left them are needed after line e: a test,
   or a carry use, before anything sets them */
static int flags_needed_after(flow_t *F, size_t e)
{
    size_t j;
    for (j = e + 1; j < F->n && j < e + 32; j++) {
        line_t *l = &F->L[j];
        if (l->kind == K_NONE)
            continue;
        if (l->kind != K_INSN || l->labelled)
            return 1;
        if (is_cond_branch(l) || is(l, "php"))
            return 1;
        if (is(l, "adc") || is(l, "sbc") || is(l, "rol") || is(l, "ror"))
            return 1;           /* the carry */
        if (is(l, "cmp") || is(l, "cpx") || is(l, "cpy"))
            return 0;           /* N, Z and C anew */
        if (is(l, "clc") || is(l, "sec")) {
            /* C anew; N and Z are still the block's until a writer */
            size_t k;
            for (k = j + 1; k < F->n && k < j + 32; k++) {
                line_t *m = &F->L[k];
                if (m->kind == K_NONE)
                    continue;
                if (m->kind != K_INSN || m->labelled || is_cond_branch(m) || is(m, "php"))
                    return 1;
                if (is(m, "lda") || is(m, "adc") || is(m, "sbc") || is(m, "and") || is(m, "ora") || is(m, "eor")
                    || is(m, "ldx") || is(m, "ldy") || is(m, "cmp"))
                    return 0;
                if (m->flow)
                    return 1;
            }
            return 1;
        }
        if (is(l, "lda") || is(l, "ldx") || is(l, "ldy") || is(l, "and") || is(l, "ora") || is(l, "eor")) {
            /* N and Z anew, C still the block's: a later carry use reads it */
            size_t k;
            for (k = j + 1; k < F->n && k < j + 32; k++) {
                line_t *m = &F->L[k];
                if (m->kind == K_NONE)
                    continue;
                if (m->kind != K_INSN || m->labelled)
                    return 1;
                if (is(m, "adc") || is(m, "sbc") || is(m, "rol") || is(m, "ror") || is_cond_branch(m) || is(m, "php"))
                    return 1;
                if (is(m, "clc") || is(m, "sec") || is(m, "cmp") || is(m, "cpx") || is(m, "cpy"))
                    return 0;
                if (is(m, "rtl") || is(m, "rts") || is(m, "jsr") || is(m, "jsl"))
                    return 0;
                if (m->flow)
                    return 1;
            }
            return 1;
        }
        if (is(l, "rtl") || is(l, "rts") || is(l, "jsr") || is(l, "jsl"))
            return 0;
        if (l->flow)
            return 1;
    }
    return 1;
}

/* A's value is not read after line e before it's loaded anew */
static int a_unread_after(flow_t *F, size_t e)
{
    static const char *noa[] = {"ldx", "ldy", "stx", "sty", "stz", "inx", "dex", "iny", "dey", "cpx", "cpy", "txy",
                                "tyx", "pea", "pei", "phx", "phy", "plx", "ply", "clc", "sec", "nop", NULL};
    size_t j;
    for (j = e + 1; j < F->n && j < e + 32; j++) {
        line_t *l = &F->L[j];
        int k, quiet = 0;
        if (l->kind == K_NONE)
            continue;
        if (l->kind != K_INSN || l->labelled)
            return 0;
        if (a_overwritten(l))
            return 1;
        if (is(l, "rtl") || is(l, "rts"))
            return 1;
        for (k = 0; noa[k]; k++)
            if (is(l, noa[k]))
                quiet = 1;
        if (!quiet)
            return 0;
    }
    return 0;
}

size_t reuseBlocks(dynArray *file)
{
    flow_t F;
    size_t i, done = 0;
    char *gone;
    char buf[MAXLEN_LINE], buf2[MAXLEN_LINE];

    if (!file->used)
        return 0;
    analyze(file, &F);
    gone = calloc(F.n, 1);
    if (!gone)
        fatal("malloc-deadstore");

    for (i = 0; i < F.n; i++) {
        size_t e1, s2, e2, len, k, j;
        int ra, rb, ok, sr, src;
        if (gone[i] || block_at(&F, i, &e1, &ra) < 0)
            continue;
        src = block_src;
        len = e1 - i + 1;
        sr = 0;
        for (k = i; k <= e1; k++)
            if (stack_relative(&F.L[k]))
                sr = 1;
        /* the next block that could repeat it, in the same straight code */
        for (s2 = e1 + 1; s2 < F.n && s2 < e1 + 40; s2++) {
            line_t *t = &F.L[s2];
            if (gone[s2] || t->kind != K_INSN || t->labelled || t->flow || t->keep)
                break;
            if (block_at(&F, s2, &e2, &rb) == 0 && e2 - s2 + 1 == len && rb != ra) {
                /* line for line, rA as rB, and neither names the other */
                ok = 1;
                for (k = 0; k < len && ok; k++) {
                    if (mentions_reg(F.code[s2 + k], ra) || mentions_reg(F.code[i + k], rb))
                        ok = 0;
                    rename_reg(F.code[i + k], ra, rb, buf, sizeof buf);
                    if (strcmp(buf, F.code[s2 + k]))
                        ok = 0;
                }
                if (!ok)
                    break;
                /* between: nothing the first block reads, or rA, changes */
                for (j = e1 + 1; j < s2 && ok; j++) {
                    line_t *m = &F.L[j];
                    int b = stored_reg(m);
                    if (b >= 0) {
                        /* a pseudo-register store: not rA, nor one the block reads */
                        for (k = i; k <= e1; k++)
                            if (mentions_reg(F.code[k], b) && b != ra)
                                ok = 0;
                        if (b == ra || b == ra + 1)
                            ok = 0;
                    } else if (writes_memory(m))
                        ok = 0;
                    if (sr && stack_op(m))
                        ok = 0;
                    if (is(m, "sep") || is(m, "rep") || m->flow || m->labelled || m->kind != K_INSN)
                        ok = 0;
                    /* the source register unchanged */
                    if (src == 'y' && (is(m, "ldy") || is(m, "iny") || is(m, "dey") || is(m, "tay") || is(m, "ply")
                                       || is(m, "txy")))
                        ok = 0;
                    if (src == 'x' && (is(m, "ldx") || is(m, "inx") || is(m, "dex") || is(m, "tax") || is(m, "plx")
                                       || is(m, "tyx")))
                        ok = 0;
                }
                /* after: A and the flags the second block left aren't needed */
                if (ok && (!a_unread_after(&F, e2) || flags_needed_after(&F, e2)))
                    ok = 0;
                /* rB's uses, until it's dead, can read rA (unchanged) */
                if (ok) {
                    size_t last = 0;
                    for (j = e2 + 1; j < F.n; j++) {
                        line_t *m = &F.L[j];
                        if (gone[j])
                            break;
                        if (m->kind != K_INSN || m->labelled) {
                            ok = 0;     /* still live at a label or directive */
                            break;
                        }
                        if (stored_reg(m) == ra || ((m->def >> ra) & 1) || stored_reg(m) == rb) {
                            ok = 0;
                            break;
                        }
                        if (m->flow && ((F.out[j] >> rb) & 1)) {
                            ok = 0;
                            break;
                        }
                        if (mentions_reg(F.code[j], rb + 1)) {
                            ok = 0;     /* rBh: not ours to rename */
                            break;
                        }
                        if (!((F.out[j] >> rb) & 1)) {
                            last = j;
                            break;
                        }
                    }
                    if (ok && last) {
                        for (j = e2 + 1; j <= last; j++)
                            if (mentions_reg(F.code[j], rb)) {
                                rename_reg(file->arr[j], rb, ra, buf2, sizeof buf2);
                                replace_line(file, j, buf2);
                            }
                        for (j = s2; j <= e2; j++) {
                            drop_line(file, j);
                            gone[j] = 1;
                        }
                        for (j = i; j <= e1; j++)
                            gone[j] = 1;
                        done++;
                    }
                }
                break;
            }
        }
    }

    free(gone);
    release(&F);
    compact(file);
    return done;
}
