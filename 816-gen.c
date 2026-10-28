/*
 *  WDC 65816 code generator for TCC
 *
 *  Copyright (c) 2007 Ulrich Hecht
 *
 *  Based on arm-gen.c by Daniel Glöckner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define LDOUBLE_SIZE 12 // not actually supported
#define LDOUBLE_ALIGN 4
#define MAX_ALIGN 8

#define NB_REGS 15

#define RC_INT 0x0001
#define RC_FLOAT 0x0002
#define RC_R0 0x0004
#define RC_R1 0x0008
#define RC_R2 0x0010
#define RC_R3 0x0020
#define RC_R4 0x0040
#define RC_R5 0x0080
#define RC_R9 0x0100
#define RC_R10 0x0200
#define RC_F0 0x0400
#define RC_F1 0x0800
#define RC_F2 0x1000
#define RC_F3 0x2000
#define RC_NONE 0x8000

#define RC_IRET RC_R0
#define RC_LRET RC_R1
#define RC_FRET RC_F0

enum {
    TREG_R0 = 0,
    TREG_R1,
    TREG_R2,
    TREG_R3,
    TREG_R4,
    TREG_R5,
    TREG_R9 = 9,
    TREG_R10,
    TREG_F0,
    TREG_F1,
    TREG_F2,
    TREG_F3,
};

int reg_classes[NB_REGS] = {
    RC_INT | RC_R0,
    RC_INT | RC_R1,
    RC_INT | RC_R2,
    RC_INT | RC_R3,
    RC_INT | RC_R4,
    RC_INT | RC_R5,
    RC_NONE,
    RC_NONE,
    RC_NONE,
    RC_R9,
    RC_R10,
    RC_FLOAT | RC_F0,
    RC_FLOAT | RC_F1,
    RC_FLOAT | RC_F2,
    RC_FLOAT | RC_F3,
};

#define REG_IRET TREG_R0
#define REG_LRET TREG_R1
#define REG_FRET TREG_F0

#define R_DATA_32 1  // whatever
#define R_DATA_PTR 1 // whatever
#define R_JMP_SLOT 2 // whatever
#define R_COPY 3     // whatever

#define ELF_PAGE_SIZE 0x1000 // whatever
#define ELF_START_ADDR 0x400 // made up

#define PTR_SIZE 4

#define EM_TCC_TARGET EM_W65

#define LOCAL_LABEL "__local_%d"

#define MAXLEN 512

#define INITIAL_LABELS_CAPACITY 64
#define INITIAL_JUMPS_CAPACITY 256
#define INITIAL_LOCALS_CAPACITY 64

char unique_token[] = "{WLA_FILENAME}";

/**
 * @note WLA does not have file-local symbols, only section-local and global.
 * Thus, everything that is file-local must be made global and given a
 * unique name. Not knowing how to choose one deterministically, we use a
 * random string, saved to STATIC_PREFIX.
 * With WLA 9.X, if you have a label that begins with a _ and it is inside a section,
 * then the name doesn't show outside of that section.
 * If it is not inside a section it doesn't show outside of the object file...
 */

#define STATIC_PREFIX "tccs_"
char current_fn[MAXLEN] = "";

// Variable relocate a given section
char **relocptrs = NULL;

char *label_workaround = NULL;

/**
 * @struct labels_816
 *
 * @brief Structure representing labels in the code.
 *
 * This structure represents a label in the code with a name and a position.
 */
struct labels_816
{
    char *name; /**< @brief The name of the label. */
    int pos;    /**< @brief The position of the label in the code. */
};

struct labels_816 *label = NULL; /**< @brief Dynamic array to store label structures. */
int labels = 0;
int labels_capacity = 0;

/** @brief Ensures the label array has enough capacity */
static void ensure_label_capacity(void)
{
    if (labels >= labels_capacity) {
        labels_capacity = labels_capacity ? labels_capacity * 2 : INITIAL_LABELS_CAPACITY;
        label = realloc(label, labels_capacity * sizeof(*label));
        if (!label)
            error("out of memory allocating labels");
    }
}

/**
 * @brief Constructs and returns a symbol string from a given symbol.
 *
 * This function constructs a string representing a given symbol. If the symbol is static,
 * a prefix is added to the symbol name. The constructed string is then returned.
 * Note that the returned string is statically allocated, and therefore will be
 * overwritten on subsequent calls to this function.
 *
 * @param sym Pointer to the symbol structure representing the symbol.
 * @return Pointer to the constructed symbol string. This string is statically allocated and should not be freed.
 */
char *get_sym_str(Sym *sym)
{
    static char name[MAXLEN];
    char *symname = get_tok_str(sym->v, NULL);
    name[0] = 0;
    /* if static, add prefix */
    if (sym->type.t & VT_STATIC) {
        if ((sym->type.t & VT_STATICLOCAL) && current_fn[0] != 0
            && !((sym->type.t & VT_BTYPE) == VT_FUNC))
            snprintf(name, MAXLEN, "%s_FUNC_%s_", STATIC_PREFIX, current_fn);
        else
            snprintf(name, MAXLEN, "%s%s_", STATIC_PREFIX, unique_token);
    }

    /* add symbol name (with bounds checking) */
    size_t current_len = strlen(name);
    size_t symname_len = strlen(symname);
    if (current_len + symname_len < MAXLEN) {
        strcat(name, symname);
    } else {
        /* truncate to fit within buffer */
        strncat(name, symname, MAXLEN - current_len - 1);
        name[MAXLEN - 1] = '\0';
    }

    return name;
}

/**
 * @brief Adds a character into the current text section and increments the index.
 *
 * This function checks if there is enough space allocated in the current text section,
 * if not, it reallocates the section with additional space. Then it copies the character
 * into the current text section at the current index and increments the index to point
 * to the next free position in the text section.
 *
 * @param c The character to be added into the current text section.
 */
void g(int c)
{
    // If there isn't enough space allocated in the current text section,
    // reallocate the section with additional space.
    if (ind >= cur_text_section->data_allocated) {
        section_realloc(cur_text_section, ind + 1);
    }

    // Copy the character into the current text section at the current index.
    memcpy(&cur_text_section->data[ind], &c, 1);

    // Increment the index to point to the next free position in the text section.
    ind++;
}

/**
 * @brief Adds each character from the input string into the current text section.
 *
 * This function iterates over each character in the input string and calls the function
 * 'g' to add it into the current text section. It continues to do this until it reaches
 * the null character that signifies the end of the string.
 *
 * @param str The input string whose characters are to be added into the current text section.
 */
void s(char *str)
{
    // Loop while the current character in the input string is not a null character.
    // The null character indicates the end of the string.
    while (*str) {
        // Call the g function with the current character in the input string
        // and then increment the input string pointer to the next character.
        g(*str++);
    }
}

char line[MAXLEN];

/**
 * @brief Formats and prints data into the current text section.
 *
 * This function uses variadic arguments to allow for input of variable data types
 * and quantities. It uses the vsnprintf function to format the data into a string
 * and then calls the function 's' to add this string into the current text section.
 *
 * @param format This is a string that contains the text to be written to the text section.
 *               It can optionally contain embedded format specifiers that will be replaced
 *               by the values specified in subsequent additional arguments and formatted as requested.
 * @param ... This indicates that the function takes a variable number of additional arguments.
 */
void pr(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    s(line);
}

// Dynamic jump array (replaces static int jump[20000][2])
int (*jump)[2] = NULL;
int jumps = 0;
int jumps_capacity = 0;

/** @brief Ensures the jump array has enough capacity */
static void ensure_jump_capacity(void)
{
    if (jumps >= jumps_capacity) {
        jumps_capacity = jumps_capacity ? jumps_capacity * 2 : INITIAL_JUMPS_CAPACITY;
        jump = realloc(jump, jumps_capacity * sizeof(*jump));
        if (!jump)
            error("out of memory allocating jumps");
    }
}

/**
 * @brief Handles the association between a jump instruction and its target address.
 *
 * This function is used to link a jump instruction at a specific location with its target address.
 * It also handles the use of labels in the code, storing their names and positions for later use.
 * If a jump instruction cannot be found at the provided location, an error message is printed.
 *
 * @param t The location of the jump instruction in the code.
 * @param a The target address for the jump instruction.
 */
void gsym_addr(int t, int a)
{
    /* code at t wants to jump to a */
    // fprintf(stderr, "gsymming t 0x%x a 0x%x\n", t, a);
    pr("; gsym_addr t %d a %d ind %d\n", t, a, ind);
    /* the label generation code sets this for us so we know when a symbol
       is a label and what its name is, so that we can remember its name
       and position so the output code can insert it correctly */
    if (label_workaround) {
        // fprintf("setting label %s to a %d (t %d)\n", label_workaround, a, t);
        ensure_label_capacity();
        label[labels].name = label_workaround;
        label[labels].pos = a;
        labels++;
        label_workaround = NULL;
    }

    // pair up the jump with the target address
    // the tcc_output_... function will add a
    // label __local_<i> at a when writing the output
    int found = 0;
    int i;

    for (i = 0; i < jumps; i++) {
        if (jump[i][0] == t) {
            jump[i][1] = a;
            found = 1;
        }
    }
    if (!found)
        pr("; ERROR no jump found to patch\n");
}

/**
 * @brief Adds a global symbol to the current instruction index.
 *
 * This function takes a global symbol represented by its token and adds it to the
 * current instruction index. The current instruction index is determined by the
 * global variable 'ind'.
 *
 * @param t The token representing the global symbol to be added.
 */
void gsym(int t)
{
    gsym_addr(t, ind);
}

int stack_back = 0;

/**
 * @brief Adjusts the stack pointer based on the function call stack and a displacement.
 *
 * This function calculates the required adjustment to the stack pointer based on
 * the current function call stack, a given displacement, and the current location.
 * If the calculated adjustment is less than 0, the function returns the function call
 * stack value. Otherwise, the function adjusts the stack pointer and returns the
 * adjusted function call stack value.
 *
 * @param fc The function call stack.
 * @param disp The displacement value.
 * @return The adjusted function call stack value.
 */
int adjust_stack(int fc, int disp)
{
    int stack_adj = fc + disp - loc - 256;

    pr("; stack adjust: fc + disp - loc - 256 %d\n", stack_adj);

    if (stack_adj < 0)
        return fc;

    stack_back = -loc + fc + stack_adj;
    pr("tsc\nclc\nadc.w #%d\ntcs\n", stack_back);
    return fc - stack_back;
}

/**
 * @brief Restores the stack pointer based on the function call stack.
 *
 * If a previous stack adjustment was made (indicated by a non-zero value of `stack_back`),
 * this function undoes that adjustment and updates the function call stack value accordingly.
 * If no previous stack adjustment was made, the function returns the function call stack value unchanged.
 *
 * @param fc The function call stack.
 * @return The updated function call stack value.
 */
int restore_stack(int fc)
{
    if (stack_back != 0) {
        pr("tsc\nsec\nsbc.w #%d\ntcs\n", stack_back);
        fc += stack_back;
        stack_back = 0;
    }
    return fc;
}

// this used to be local to gfunc_call, but we need it to get the correct
// stack pointer displacement while building the argument stack for
// a function call
int args_size = 0;

static int jcc_begin(int cond);

/* fastcall / regparm(n): the first 16-bit (or smaller) parameters arrive in
   the direct page slots tcc__a0..a3 (crt0's register page) instead of on
   the stack. The caller stores them there last, after every argument is
   ready, and pushes only the rest. The callee reads them there: a
   parameter is a VT_LOCAL lvalue at offset FC_SLOT + 2 * k. A callee that
   makes calls itself copies them to its frame on entry (calls reuse the
   slots), writes them through to that copy, puts them back after each call,
   and reads its copy while setting up a call's slot arguments. Whether it
   makes calls is known at its end: __<fn>_psave says so to the assembler,
   as __<fn>_locals does the frame size. */
#define FC_SLOT 0x7F00
#define FC_MAX 4
#define FC_IS_SLOT(c) ((unsigned) (c) >= FC_SLOT && (unsigned) (c) < FC_SLOT + 2 * FC_MAX)
static int fc_n;                /* the current function's slot parameters */
static int fc_calls;            /* it has made a call */
static int fc_args_ctx;         /* setting up slot arguments: read the frame copy */
char **fc_defs = NULL;          /* the .defines tccelf.c puts at the top */
int fc_ndefs = 0, fc_defs_cap = 0;

static void fc_add_def(const char *fmt, ...)
{
    char buf[MAXLEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (fc_ndefs == fc_defs_cap) {
        fc_defs_cap = fc_defs_cap ? fc_defs_cap * 2 : 32;
        fc_defs = realloc(fc_defs, fc_defs_cap * sizeof *fc_defs);
        if (!fc_defs)
            error("out of memory");
    }
    fc_defs[fc_ndefs++] = strdup(buf);
}

/* how many slots a calling convention uses */
static int fc_nslots(int func_call)
{
    if (func_call == FUNC_FASTCALLW)
        return FC_MAX;
    if (func_call >= FUNC_FASTCALL1 && func_call <= FUNC_FASTCALL3)
        return func_call - FUNC_FASTCALL1 + 1;
    return 0;
}

/* a parameter that can live in a slot: 2 bytes or less, not float */
static int fc_fits(CType *t)
{
    int align, bt = t->t & VT_BTYPE;
    if (is_float(t->t) || bt == VT_STRUCT || bt == VT_LLONG || bt == VT_PTR || (t->t & VT_ARRAY))
        return 0;
    return type_size(t, &align) <= 2;
}

/* Locals in the direct page: tcc__l0..l3, callee-saved. Before a function
   is compiled, dp_prescan looks over its body's tokens and picks up to
   DP_MAX names used most inside loops (twice at least, never after a '&',
   not called, not a member); the first declaration of each, if it's a
   scalar of 2 bytes or less, lives in a slot: a VT_LOCAL lvalue at offset
   DP_SLOT + 2 * k. A function saves the slots it used on entry and puts
   them back on exit (__<fn>_dpu<k> / __<fn>_dps<k>, defined at its end),
   so callees, recursion and the vblank callback (its own page) leave them
   alone. The prescan only chooses: getting it wrong costs speed, not
   correctness. */
#define DP_SLOT 0x7E00
#define DP_MAX 4
#define DP_IS_SLOT(c) ((unsigned) (c) >= DP_SLOT && (unsigned) (c) < DP_SLOT + 2 * DP_MAX)
static int dp_cand[DP_MAX];     /* the chosen names (token ids) */
static int dp_ncand;
static int dp_used[DP_MAX];     /* a declaration got the slot */

void dp_prescan(int *ids, int n)
{
    int *depth = calloc(n + 1, sizeof *depth);
    int *weight, i, k;
    if (!depth)
        error("out of memory");
    dp_ncand = 0;
    for (k = 0; k < DP_MAX; k++)
        dp_used[k] = 0;

    /* the loops' extents: for/while from the keyword through the header
       and the body (a block, or up to its ';'); do through its body */
    for (i = 0; i < n; i++) {
        int b, end = -1, lvl;
        if (ids[i] != TOK_FOR && ids[i] != TOK_WHILE && ids[i] != TOK_DO)
            continue;
        b = i + 1;
        if (ids[i] != TOK_DO) {
            if (b >= n || ids[b] != '(')
                continue;
            for (lvl = 0; b < n; b++) {
                if (ids[b] == '(')
                    lvl++;
                else if (ids[b] == ')' && --lvl == 0)
                    break;
            }
            b++;
        }
        if (b >= n)
            continue;
        if (ids[b] == '{') {
            for (lvl = 0, end = b; end < n; end++) {
                if (ids[end] == '{')
                    lvl++;
                else if (ids[end] == '}' && --lvl == 0)
                    break;
            }
        } else {
            int par = 0, br = 0;
            for (end = b; end < n; end++) {
                if (ids[end] == '(')
                    par++;
                else if (ids[end] == ')')
                    par--;
                else if (ids[end] == '{')
                    br++;
                else if (ids[end] == '}')
                    br--;
                else if (ids[end] == ';' && par <= 0 && br <= 0)
                    break;
            }
        }
        depth[i]++;
        if (end < n)
            depth[end + 1]--;
    }
    for (i = 1; i < n; i++)
        depth[i] += depth[i - 1];

    /* weigh each name: 1 outside a loop, 8 in one, 32 deeper */
    weight = calloc(n, sizeof *weight);
    if (!weight)
        error("out of memory");
    for (i = 0; i < n; i++) {
        int t = ids[i], j, w, excluded = 0;
        if (t < TOK_UIDENT)
            continue;
        if (i + 1 < n && ids[i + 1] == '(')
            excluded = 1;                       /* a call */
        if (i > 0 && (ids[i - 1] == '.' || ids[i - 1] == TOK_ARROW))
            excluded = 1;                       /* a member */
        for (j = i - 1; j >= 0 && ids[j] == '('; j--)
            ;
        if (j >= 0 && ids[j] == '&')
            excluded = 2;                       /* its address, maybe */
        w = depth[i] == 0 ? 1 : depth[i] == 1 ? 8 : 32;
        for (j = 0; j < i; j++)
            if (ids[j] == t)
                break;
        /* keep the totals on the name's first occurrence */
        if (weight[j] >= 0)
            weight[j] = excluded == 2 ? -1 : (excluded ? weight[j] : weight[j] + w);
        if (excluded == 2)
            weight[j] = -1;
    }
    /* the heaviest DP_MAX, 16 at least (two uses in a loop) */
    for (k = 0; k < DP_MAX; k++) {
        int best = -1, bw = 15;
        for (i = 0; i < n; i++)
            if (weight[i] > bw) {
                bw = weight[i];
                best = i;
            }
        if (best < 0)
            break;
        dp_cand[dp_ncand++] = ids[best];
        weight[best] = 0;
    }
    free(depth);
    free(weight);
}

/* the slot for a local being declared, or -1 */
int dp_slot_for(int v, CType *type)
{
    int k;
    if (!fc_fits(type) || (type->t & VT_VOLATILE))
        return -1;
    for (k = 0; k < dp_ncand; k++)
        if (dp_cand[k] == (v & ~SYM_FIELD) && !dp_used[k]) {
            dp_used[k] = 1;
            return k;
        }
    return -1;
}

/* a slot parameter's operand: the slot, or the frame copy while setting
   up a call's slot arguments */
static void fc_operand(char *buf, size_t n, int k)
{
    if (fc_args_ctx)
        snprintf(buf, n, " __%s_ps%d + %d + __%s_locals + 1,s", current_fn, k, args_size, current_fn);
    else
        snprintf(buf, n, ".b tcc__a%d", k);
}

/* An indexed global: a register value whose r has VT_IDXSYM is
   sym + c.i + (that register, a 16-bit byte offset). With VT_LVAL it is
   the element there: ldx.b tcc__rN / lda.l sym+c,x. Without, the address,
   made into a pointer only when something needs one. The flag borrows
   VT_BOUNDED (bounds checking, which this target doesn't use). The long
   indexed address carries into the next bank, where the old pointer add
   wrapped in it: the same for any index inside a C array. */
#define VT_IDXSYM VT_BOUNDED

static int is_idx(SValue *v)
{
    return (v->r & VT_IDXSYM) && (v->r & VT_VALMASK) < VT_CONST;
}

static int is_idx_addr(SValue *v)
{
    return is_idx(v) && !(v->r & VT_LVAL);
}

static int is_plain_const(SValue *v)
{
    int bt = v->type.t & VT_BTYPE;
    return (v->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && bt != VT_LLONG && !is_float(v->type.t);
}

static int is_sym_addr(SValue *v)
{
    return (v->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM) && (v->type.t & VT_BTYPE) == VT_PTR;
}

static int is_int_val(SValue *v)
{
    int bt = v->type.t & VT_BTYPE;
    if (is_idx(v) || (v->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
        return 0;
    return bt == VT_INT || bt == VT_SHORT || bt == VT_BYTE || bt == VT_BOOL;
}

/* an indexed global's address into register r as a pointer (the bank
   from the symbol, as for any symbol's address) */
static void idx_to_ptr(int r, SValue *sv)
{
    char *sy = get_sym_str(sv->sym);
    pr("; idx addr [%s + %d + tcc__r%d] -> tcc__r%d\n", sy, sv->c.i, sv->r & VT_VALMASK, r);
    pr("lda.w #%s + %d\nclc\nadc.b tcc__r%d\nsta.b tcc__r%d\nlda.w #:%s\nsta.b tcc__r%dh\n",
       sy, sv->c.i, sv->r & VT_VALMASK, r, sy, r);
}

int ll_workaround = 0;

/**
 * @brief The function loads a value from memory or a register into a specified register.
 *
 * @param r The register number to load the value into.
 * @param sv The parameter `sv` is a pointer to a `SValue` struct, which contains information about the
 * value being loaded into a register. This includes the register to load into (`sv->r`), the type of
 * the value (`sv->type.t`), and any additional information about the value.
 */
void load(int r, SValue *sv)
{
    int fr, ft, fc;
    int length;
    int align;
    int v, sign, t;
    SValue v1;
    pr("; load %d\n", r);
    pr("; type %d reg 0x%x extra 0x%x\n", sv->type.t, sv->r, sv->type.extra);
    fr = sv->r;
    ft = sv->type.t;
    fc = sv->c.ul;

    length = type_size(&sv->type, &align);
    if ((ft & VT_BTYPE) == VT_LLONG)
        length = 2; // long longs are handled word-wise
    if (ll_workaround)
        length = 4;

    int base = -1;
    v = fr & VT_VALMASK;
    if (is_idx(sv)) {
        char *sy = get_sym_str(sv->sym);
        if (!(fr & VT_LVAL)) {      /* the address */
            idx_to_ptr(r, sv);
            return;
        }
        /* the element: as a symbol's, indexed by X */
        pr("; ld%d [%s + %d + tcc__r%d], r%d\n", length, sy, fc, v, r);
        pr("ldx.b tcc__r%d\n", v);
        if (is_float(ft)) {
            if (length != 4)
                error("ICE idx float");
            pr("lda.l %s + %d,x\nsta.b tcc__f%d\nlda.l %s + %d + 2,x\nsta.b tcc__f%dh\n",
               sy, fc, r - TREG_F0, sy, fc, r - TREG_F0);
            return;
        }
        switch (length) {
        case 1:
            pr("lda.w #0\nsep #$20\nlda.l %s + %d,x\nrep #$20\n", sy, fc);
            if (!(ft & VT_UNSIGNED))
                pr("xba\nxba\nbpl +\nora.w #$ff00\n+\n");
            pr("sta.b tcc__r%d\n", r);
            break;
        case 2:
            pr("lda.l %s + %d,x\nsta.b tcc__r%d\n", sy, fc, r);
            break;
        case 4:
            pr("lda.l %s + %d,x\nsta.b tcc__r%d\nlda.l %s + %d + 2,x\nsta.b tcc__r%dh\n", sy, fc, r, sy, fc, r);
            break;
        default:
            error("ICE idx load %d", length);
        }
        return;
    }
    if (v == VT_LOCAL && DP_IS_SLOT(fc)) {      /* a local in the direct page */
        int k = (fc - DP_SLOT) / 2;
        if (!(fr & VT_LVAL))
            error("ICE: the address of a direct page local");
        pr("; ld%d dp local %d -> tcc__r%d\n", length, k, r);
        if (length == 1) {
            pr("lda.w #0\nsep #$20\nlda.b tcc__l%d\nrep #$20\n", k);
            if (!(ft & VT_UNSIGNED))
                pr("xba\nxba\nbpl +\nora.w #$ff00\n+\n");
        } else
            pr("lda.b tcc__l%d\n", k);
        pr("sta.b tcc__r%d\n", r);
        return;
    }
    if (v == VT_LOCAL && FC_IS_SLOT(fc)) {      /* a fastcall parameter */
        char src[MAXLEN];
        if (!(fr & VT_LVAL))
            error("can't take the address of a fastcall parameter (copy it to a local)");
        fc_operand(src, sizeof src, (fc - FC_SLOT) / 2);
        pr("; ld%d fastcall param %d -> tcc__r%d\n", length, (fc - FC_SLOT) / 2, r);
        if (length == 1) {
            pr("lda.w #0\nsep #$20\nlda%s\nrep #$20\n", src);
            if (!(ft & VT_UNSIGNED))
                pr("xba\nxba\nbpl +\nora.w #$ff00\n+\n");
        } else
            pr("lda%s\n", src);
        pr("sta.b tcc__r%d\n", r);
        return;
    }
    if (fr & VT_LVAL) {
        if (v == VT_LLOCAL) {
            v1.type.t = VT_PTR;
            v1.r = VT_LOCAL | VT_LVAL;
            v1.c.ul = sv->c.ul;
            load(base = 10 /* lr */, &v1);
            fc = sign = 0;
            v = VT_LOCAL;
        } else if (v == VT_CONST) {
            if (fr & VT_SYM) { // deref symbol + displacement
                char *sy = get_sym_str(sv->sym);
                if (is_float(ft)) {
                    pr("; fld%d [%s + %d], tcc__f%d\n", length, sy, fc, r - TREG_F0);
                    switch (length) {
                    case 4:
                        pr("lda.l %s + %d\nsta.b tcc__f%d\nlda.l %s + %d + 2\nsta.b tcc__f%dh\n",
                           sy,
                           fc,
                           r - TREG_F0,
                           sy,
                           fc,
                           r - TREG_F0);
                        break;
                    default:
                        error("ICE 1");
                    }
                } else {
                    pr("; ld%d [%s + %d], tcc__r%d\n", length, sy, fc, r);
                    // Large offsets may cross bank boundaries on 65816
                    // lda.l supports 24-bit addressing, assembler will compute full address
                    if (fc > 65535)
                        warning("large index %d may cross bank boundary", fc);
                    switch (length) {
                    case 1:
                        pr("lda.w #0\nsep #$20\nlda.l %s + %d\nrep #$20\n", sy, fc);
                        if (!(ft & VT_UNSIGNED))
                            pr("xba\nxba\nbpl +\nora.w #$ff00\n+\n");
                        pr("sta.b tcc__r%d\n", r);
                        break;
                    case 2:
                        pr("lda.l %s + %d\nsta.b tcc__r%d\n", sy, fc, r);
                        break;
                    case 4:
                        pr("lda.l %s + %d\nsta.b tcc__r%d\nlda.l %s + %d + 2\nsta.b tcc__r%dh\n",
                           sy,
                           fc,
                           r,
                           sy,
                           fc,
                           r);
                        break;
                    default:
                        error("ICE 1");
                    }
                }
            } else { // deref constant pointer
                // error("ld [%d],tcc__r%d\n",fc,r);
                pr("; deref constant ptr ld [%d],tcc__r%d\n", fc, r);
                if (is_float(ft)) {
                    error("dereferencing constant float pointers unimplemented\n");
                } else {
                    switch (length) {
                    case 1:
                        pr("lda.w #0\nsep #$20\nlda.l %d\nrep #$20\n", fc);
                        if (!(ft & VT_UNSIGNED))
                            pr("xba\nxba\nbpl +\nora.w #$ff00\n+\n");
                        pr("sta.b tcc__r%d\n", r);
                        break;
                    case 2:
                        pr("lda.l %d\nsta.b tcc__r%d\n", fc, r);
                        break;
                    case 4:
                        pr("lda.l %d\nsta.b tcc__r%d\nlda.l %d + 2\nsta.b tcc__r%dh\n", fc, r, fc, r);
                        break;
                    default:
                        error("ICE 1");
                    }
                }
            }
            return;
        } else if (v < VT_CONST) { // deref pointer in register
            base = v;
            fc = sign = 0;
            v = VT_LOCAL;
        }

        if (v == VT_LOCAL) {
            if (is_float(ft)) {
                if (base == -1) {
                    pr("; fld%d [sp,%d],tcc__f%d\n", length, fc, r - TREG_F0);
                    if (length != 4)
                        error("ICE 2f");
                    fc = adjust_stack(fc, args_size + 2);
                    pr("lda %d + __%s_locals + 1,s\nsta.b tcc__f%d\nlda %d + __%s_locals + "
                       "1,s\nsta.b tcc__f%dh\n",
                       fc + args_size,
                       current_fn,
                       r - TREG_F0,
                       fc + args_size + 2,
                       current_fn,
                       r - TREG_F0);
                    fc = restore_stack(fc);
                } else {
                    pr("; fld%d [tcc__r%d,%d],tcc__f%d\n", length, base, fc, r - TREG_F0);
                    if (length != 4)
                        error("ICE 3f");
                    pr("ldy #%d\nlda.b [tcc__r%d],y\nsta.b tcc__f%d\niny\niny\nlda.b [tcc__r%d], "
                       "y\nsta.b tcc__f%dh\n",
                       fc,
                       base,
                       r - TREG_F0,
                       base,
                       r - TREG_F0);
                }
            } else {
                if (base == -1) { // value of local at fc
                    pr("; ld%d [sp,%d],tcc__r%d\n", length, fc, r);
                    fc = adjust_stack(fc, args_size + 2);
                    switch (length) {
                    case 1:
                        pr("lda.w #0\nsep #$20\nlda %d + __%s_locals + 1,s\nrep #$20\n",
                           fc + args_size,
                           current_fn);
                        if (!(ft & VT_UNSIGNED))
                            pr("xba\nxba\nbpl +\nora.w #$ff00\n+\n");
                        pr("sta.b tcc__r%d\n", r);
                        break;
                    case 2:
                        pr("lda %d + __%s_locals + 1,s\nsta.b tcc__r%d\n",
                           fc + args_size,
                           current_fn,
                           r);
                        break;
                    case 4:
                        pr("lda %d + __%s_locals + 1,s\nsta.b tcc__r%d\nlda %d + __%s_locals + "
                           "1,s\nsta.b tcc__r%dh\n",
                           fc + args_size,
                           current_fn,
                           r,
                           fc + args_size + 2,
                           current_fn,
                           r);
                        break;
                    default:
                        error("ICE 2");
                        break;
                    }
                    fc = restore_stack(fc);
                } else { // value of array member r[fc]
                    pr("; ld%d [tcc__r%d,%d],tcc__r%d\n", length, base, fc, r);
                    switch (length) {
                    case 1:
                        pr("lda.w #0\n");
                        if (!fc)
                            pr("sep #$20\nlda.b [tcc__r%d]\nrep #$20\n", base);
                        else
                            pr("ldy #%d\nsep #$20\nlda.b [tcc__r%d],y\nrep #$20\n", fc, base);
                        if (!(ft & VT_UNSIGNED))
                            pr("xba\nxba\nbpl +\nora.w #$ff00\n+\n");
                        pr("sta.b tcc__r%d\n", r);
                        break;
                    case 2:
                        if (!fc)
                            pr("lda.b [tcc__r%d]\nsta.b tcc__r%d\n", base, r);
                        else
                            pr("ldy #%d\nlda.b [tcc__r%d],y\nsta.b tcc__r%d\n", fc, base, r);
                        break;
                    case 4:
                        pr("ldy #%d\nlda.b [tcc__r%d],y\nsta.b tcc__r%d\niny\niny\nlda.b "
                           "[tcc__r%d],y\nsta.b tcc__r%dh\n",
                           fc,
                           base,
                           r,
                           base,
                           r);
                        break;
                    default:
                        error("ICE 3");
                        break;
                    }
                }
            }
            return;
        }
    } else { // VT_LVAL
        if (v == VT_CONST) {
            if (fr & VT_SYM) { // symbolic constant
                char *sy = get_sym_str(sv->sym);
                pr("; ld%d #%s + %d, tcc__r%d (type 0x%x)\n", length, sy, fc, r, ft);
                if (length != PTR_SIZE)
                    pr("; FISHY! length <> PTR_SIZE! (may be an array)\n");
                pr("lda.w #:%s\nsta.b tcc__r%dh\nlda.w #%s + %d\nsta.b tcc__r%d\n", sy, r, sy, fc, r);
            } else { // numeric constant
                pr("; ld%d #%d,tcc__r%d\n", length, sv->c.ul, r);
                if ((ft & VT_BTYPE) == VT_BOOL) {
                    sv->c.ul = sv->c.ul ? 1 : 0;
                }
                switch (length) {
                case 1:
                    if (ft & VT_UNSIGNED) {
                        pr("lda.w #%d\n", sv->c.ul & 0xff);
                    } else {
                        pr("lda.w #%d\n", ((short) ((sv->c.ul & 0xff) << 8)) >> 8);
                    }
                    pr("sta.b tcc__r%d\n", r);
                    break;
                case 2:
                    pr("lda.w #%d\nsta.b tcc__r%d\n", sv->c.ul & 0xffff, r);
                    break;
                case 4:
                    pr("lda.w #%d\nsta.b tcc__r%d\nlda.w #%d\nsta.b tcc__r%dh\n",
                       sv->c.ul & 0xffff,
                       r,
                       sv->c.ul >> 16,
                       r);
                    break;
                default:
                    error("ICE 4");
                }
            }
            return;
        } else if (v == VT_LOCAL) {
            if (fr & VT_SYM) {
                error("symbol");
                char *sy = get_sym_str(sv->sym);
                pr("; LOCAL ld%d #%s, tcc__r%d (type 0x%x)\n", length, sy, r, ft);
            } else { // local pointer
                pr("; ld%d #(sp) + %d,tcc__r%d (fr 0x%x ft 0x%x fc 0x%x)\n",
                   length,
                   sv->c.ul,
                   r,
                   fr,
                   ft,
                   fc);
                // pointer; have to ensure the upper word is correct (page 0)
                pr("stz.b tcc__r%dh\ntsa\nclc\nadc #(%d + __%s_locals + 1)\nsta.b tcc__r%d\n",
                   r,
                   sv->c.ul + args_size,
                   current_fn,
                   r);
            }
            return;
        } else if (v == VT_CMP) {
            /* a compare's flags to 0 or 1 */
            int n = jcc_begin(sv->c.i);
            pr("__ct%d:\nlda.w #1\nbra __ce%d\n__cs%d:\nlda.w #0\n__ce%d:\nsta.b tcc__r%d\n", n, n, n, n, r);
            return;
        } else if (v == VT_JMP || v == VT_JMPI) {
            t = v & 1; // inverted or not
            pr("; jmpr(i) v 0x%x r 0x%x fc 0x%x\n", v, r, fc);
            pr("lda #%d\nbra +\n", t);
            gsym(fc);
            pr("lda #%d\n+\nsta.b tcc__r%d\n",
               t ^ 1,
               r); // stz rXh seems to be unnecessary (we only look at the lower word)
            return;
        } else if (v < VT_CONST) { // register value
            if (is_float(ft)) {
                v -= TREG_F0;
                r -= TREG_F0;
                pr("; fmov tcc__f%d, tcc__f%d\n", v, r);
                pr("lda.b tcc__f%d\nsta.b tcc__f%d\nlda.b tcc__f%dh\nsta.b tcc__f%dh\n", v, r, v, r);
            } else {
                pr("; mov tcc__r%d, tcc__r%d\n", v, r);
                pr("lda.b tcc__r%d\nsta.b tcc__r%d\nlda.b tcc__r%dh\nsta.b tcc__r%dh\n", v, r, v, r);
            }
            return;
        }
    }
    error("load unimplemented");
}

/**
 * @brief Store a value at a specified location.
 *
 * This function is used to store a value from a given register into a specified
 * location. The location and the value are given by a SValue structure. The function
 * can handle different types of data such as integers, long longs, symbols and
 * pointers, among others. The stored value is written in the assembly language.
 * Errors are thrown in case of illegal or unimplemented operations.
 *
 * @param r    Register from which the value is to be stored.
 * @param sv   Pointer to a structure of type SValue which holds the value and its
 *             location to be stored.
 */
void store(int r, SValue *sv)
{
    int v, ft, fc, fr, sign;
    int base;
    int length, align;
    SValue v1;

    fr = sv->r;
    ft = sv->type.t;
    fc = sv->c.i;

    length = type_size(&sv->type, &align);
    if ((ft & VT_BTYPE) == VT_LLONG)
        length = 2; // long longs are handled word-wise

    pr("; store r 0x%x fr 0x%x ft 0x%x fc 0x%x\n", r, fr, ft, fc);

    if (is_idx(sv) && (fr & VT_LVAL)) {     /* an indexed global's element */
        char *sy = get_sym_str(sv->sym);
        pr("ldx.b tcc__r%d\n", fr & VT_VALMASK);
        switch (length) {
        case 1:
            pr("sep #$20\nlda.b tcc__r%d\nsta.l %s + %d,x\nrep #$20\n", r, sy, fc);
            break;
        case 2:
            pr("lda.b tcc__r%d\nsta.l %s + %d,x\n", r, sy, fc);
            break;
        case 4:
            if (r >= TREG_F0)
                pr("lda.b tcc__f%d\nsta.l %s + %d,x\nlda.b tcc__f%dh\nsta.l %s + %d + 2,x\n",
                   r - TREG_F0, sy, fc, r - TREG_F0, sy, fc);
            else
                pr("lda.b tcc__r%d\nsta.l %s + %d,x\nlda.b tcc__r%dh\nsta.l %s + %d + 2,x\n", r, sy, fc, r, sy, fc);
            break;
        default:
            error("ICE idx store %d", length);
        }
        return;
    }
    if ((fr & VT_VALMASK) == VT_LOCAL && (fr & VT_LVAL) && DP_IS_SLOT(fc)) {   /* a direct page local */
        int k = (fc - DP_SLOT) / 2;
        pr("; st%d tcc__r%d -> dp local %d\n", length, r, k);
        if (length == 1)
            pr("sep #$20\nlda.b tcc__r%d\nsta.b tcc__l%d\nrep #$20\n", r, k);
        else
            pr("lda.b tcc__r%d\nsta.b tcc__l%d\n", r, k);
        return;
    }
    if ((fr & VT_VALMASK) == VT_LOCAL && (fr & VT_LVAL) && FC_IS_SLOT(fc)) {   /* a fastcall parameter */
        int k = (fc - FC_SLOT) / 2;
        pr("; st%d tcc__r%d -> fastcall param %d\n", length, r, k);
        if (length == 1)
            pr("sep #$20\n");
        pr("lda.b tcc__r%d\nsta.b tcc__a%d\n", r, k);
        /* and its frame copy, when the function makes calls */
        pr(".ifgr __%s_psave 0\nsta __%s_ps%d + %d + __%s_locals + 1,s\n.endif\n",
           current_fn, current_fn, k, args_size, current_fn);
        if (length == 1)
            pr("rep #$20\n");
        return;
    }

    v = fr & VT_VALMASK;
    base = -1;
    if ((fr & VT_LVAL) || fr == VT_LOCAL) {
        if (v < VT_CONST) { // deref register
            base = v;
            v = VT_LOCAL;
            fc = sign = 0; // no clue...
        }
        if (v == VT_CONST) {
            if (fr & VT_SYM) { // deref symbol
                char *sy = get_sym_str(sv->sym);
                if (r >= TREG_F0)
                    pr("; fst%d tcc__f%d, [%s,%d]\n", length, r - TREG_F0, sy, fc);
                else
                    pr("; st%d tcc__r%d, [%s,%d]\n", length, r, sy, fc);
                if (r >= TREG_F0 && length != 4)
                    error("illegal float store of length %d", length);
                switch (length) {
                case 1:
                    pr("sep #$20\nlda.b tcc__r%d\nsta.l %s + %d\nrep #$20\n", r, sy, fc);
                    break;
                case 2:
                    pr("lda.b tcc__r%d\nsta.l %s + %d\n", r, sy, fc);
                    break;
                case 4:
                    if (r >= TREG_F0)
                        pr("lda.b tcc__f%d\nsta.l %s + %d\nlda.b tcc__f%dh\nsta.l %s + %d + 2\n",
                           r - TREG_F0,
                           sy,
                           fc,
                           r - TREG_F0,
                           sy,
                           fc);
                    else
                        pr("lda.b tcc__r%d\nsta.l %s + %d\nlda.b tcc__r%dh\nsta.l %s + %d + 2\n",
                           r,
                           sy,
                           fc,
                           r,
                           sy,
                           fc);
                    break;
                default:
                    error("ICE 5");
                    break;
                }
                return;
            } else {
                v1.type.t = VT_PTR; // ft;
                v1.r = fr & ~VT_LVAL;
                v1.c.ul = sv->c.ul;
                v1.sym = sv->sym;
                load(base = 9, &v1);
                fc = sign = 0;
                v = VT_LOCAL;
            }
        }
        if (v == VT_LOCAL) {
            if (r >= TREG_F0) { // is_float(ft)) {
                if (base < 0) {
                    pr("; fst%d tcc__f%d, [sp,%d]\n", length, r - TREG_F0, fc);
                    fc = adjust_stack(fc, args_size + 2);
                    switch (length) {
                    case 4:
                        pr("lda.b tcc__f%d\nsta %d + __%s_locals + 1,s\nlda.b tcc__f%dh\nsta %d + "
                           "__%s_locals + 1,s\n",
                           r - TREG_F0,
                           fc + args_size,
                           current_fn,
                           r - TREG_F0,
                           fc + args_size + 2,
                           current_fn);
                        break;
                    default:
                        error("ICE 6f");
                        break;
                    }
                    fc = restore_stack(fc);
                } else {
                    pr("; fst%d tcc__f%d, [tcc__r%d,%d]\n", length, r - TREG_F0, base, fc);
                    switch (length) {
                    case 4:
                        pr("ldy.w #0\nlda.b tcc__f%d\nsta.b [tcc__r%d],y\niny\niny\nlda.b "
                           "tcc__f%dh\nsta.b [tcc__r%d],y\n",
                           r - TREG_F0,
                           base,
                           r - TREG_F0,
                           base);
                        break;
                    default:
                        error("ICE 7f");
                        break;
                    }
                }
                return;
            } else {
                if (base < 0) { // write to local at fc
                    pr("; st%d tcc__r%d, [sp,%d]\n", length, r, fc);
                    fc = adjust_stack(fc, args_size + 2);
                    switch (length) {
                    case 1:
                        pr("sep #$20\nlda.b tcc__r%d\nsta %d + __%s_locals + 1,s\nrep #$20\n",
                           r,
                           fc + args_size,
                           current_fn);
                        break;
                    case 2:
                        pr("lda.b tcc__r%d\nsta %d + __%s_locals + 1,s\n",
                           r,
                           fc + args_size,
                           current_fn);
                        break;
                    case 4:
                        pr("lda.b tcc__r%d\nsta %d + __%s_locals + 1,s\nlda.b tcc__r%dh\nsta %d + "
                           "__%s_locals + 1,s\n",
                           r,
                           fc + args_size,
                           current_fn,
                           r,
                           fc + args_size + 2,
                           current_fn);
                        break;
                    default:
                        error("ICE 6");
                        break;
                    }
                    fc = restore_stack(fc);
                } else { // write to array member r[fc]
                    pr("; st%d tcc__r%d, [tcc__r%d,%d]\n", length, r, base, fc);
                    switch (length) {
                    case 1:
                        pr("sep #$20\nlda.b tcc__r%d\n", r);
                        if (!fc)
                            pr("sta.b [tcc__r%d]\nrep #$20\n", base);
                        else
                            pr("ldy #%d\nsta.b [tcc__r%d],y\nrep #$20\n", fc, base);
                        break;
                    case 2:
                        pr("lda.b tcc__r%d\n", r);
                        if (!fc)
                            pr("sta.b [tcc__r%d]\n", base);
                        else
                            pr("ldy #%d\nsta.b [tcc__r%d],y\n", fc, base);
                        break;
                    case 4:
                        pr("lda.b tcc__r%d\nldy #%d\nsta.b [tcc__r%d],y\nlda.b "
                           "tcc__r%dh\niny\niny\nsta.b [tcc__r%d],y\n",
                           r,
                           fc,
                           base,
                           r,
                           base);
                        break;
                    default:
                        error("ICE 7");
                        break;
                    }
                }
            }
            return;
        }
    }
    error("store unimplemented");
}

/**
 * @brief Generate function call with a specified number of arguments.
 *
 * This function manages the invocation of another function with a specified number of arguments.
 * Arguments are processed and prepared for the call, with special handling for different types such as
 * floats and structs. Both direct and symbolic function calls are supported.
 *
 * The function also maintains stack integrity and handles calling conventions correctly. If the function
 * call does not conform to the standard calling convention, necessary adjustments to the stack pointer
 * are performed after the function invocation.
 *
 * @note If the called function conforms to the 'fastcall' convention, an error will be raised.
 *
 * @param[in] nb_args The number of arguments to pass to the function.
 */
void gfunc_call(int nb_args)
{
    int align, r, i, func_call;
    Sym *func_sym;

    int length;

    /* args_size is the size of the function call arguments already
       pushed on the stack. needed so that loads and stores to
       locals on the stack still work while building an argument
       list. needs to be restored before returning to make
       nested function calls work (passing structs by value causes
       a memcpy call) */
    int restore_args_size = args_size;

    /* which arguments go in fastcall slots: the callee's first fitting
       parameters, the same rule gfunc_prolog applies */
    int slot_of[256], pos, k;
    {
        Sym *fs = vtop[-nb_args].type.ref, *p;
        int ns = fs ? fc_nslots(FUNC_CALL(fs->r)) : 0;
        for (pos = 0; pos < nb_args && pos < 256; pos++)
            slot_of[pos] = -1;
        k = 0;
        for (p = fs ? fs->next : NULL, pos = 0; p && pos < nb_args && pos < 256; p = p->next, pos++)
            if (k < ns && fc_fits(&p->type))
                slot_of[pos] = k++;
    }
    fc_calls = 1;

    for (i = 0; i < nb_args; i++) {
        pos = nb_args - 1 - i;
        if (pos < 256 && slot_of[pos] >= 0) {
            /* into its slot, after every argument is ready (they are: the
               caller's own slot parameters are read from its frame copy,
               which the stores below can't touch) */
            fc_args_ctx = 1;
            r = gv(RC_INT);
            fc_args_ctx = 0;
            pr("; fastcall arg %d -> tcc__a%d\n", pos, slot_of[pos]);
            pr("lda.b tcc__r%d\nsta.b tcc__a%d\n", r, slot_of[pos]);
            vtop--;
            continue;
        }
        length = type_size(&vtop->type, &align);
        if (vtop->type.t & VT_ARRAY)
            length = PTR_SIZE;

        if ((vtop->type.t & VT_BTYPE) == VT_STRUCT) {
            /* allocate the necessary size on stack */
            pr("; sub sp, #%d\n", length);
            pr("tsa\nsec\nsbc #%d\ntas\n", length);
            args_size += length;

            /* generate structure store */
            r = get_reg(RC_INT);
            /* put the pointer to struct store on the stack
               always remember: the 65xxs are off-by-one processors
               there is always something to increment or decrement to
               get the value you actually want. (cf. mvn/mvp) */
            pr("stz.b tcc__r%dh\ntsa\nina\nsta.b tcc__r%d\n", r, r);

            /* here, TCC generates a memcpy call
               this recursion makes the restore_args_size stuff necessary */
            vset(&vtop->type, r | VT_LVAL, 0);
            vswap();
            vstore();
        } else if (is_float(vtop->type.t)) {
            if (length != 4)
                error("unknown float size %d\n", length);
            r = gv(RC_FLOAT);
            pr("; fldpush%d (type 0x%x reg 0x%x) tcc__f%d\n",
               length,
               vtop->type.t,
               vtop->r,
               r - TREG_F0);
            pr("pei (tcc__f%dh)\npei (tcc__f%d)\n", r - TREG_F0, r - TREG_F0);
            args_size += length;
        } else {
            /* simple type (currently always same size) */
            /* XXX: implicit cast ? */
            if (((vtop->r & VT_VALMASK) == VT_CONST) && ((vtop->r & VT_LVAL) == 0)) {
                // push immediate
                pr("; push%d imm r 0x%x\n", length, vtop->r);
                if (vtop->r & VT_SYM) {
                    char *sy = get_sym_str(vtop->sym);
                    if (length != PTR_SIZE)
                        pr("; FISHY! length <> PTR_SIZE! (may be an array)\n");
                    pr("pea.w :%s\npea.w %s %c %d\n",
                       sy,
                       sy,
                       vtop->c.i < 0 ? '-' : '+',
                       abs(vtop->c.i));
                    length = PTR_SIZE;
                } else {
                    switch (length) {
                    case 1:
                        pr("sep #$20\nlda #%d\npha\nrep #$20\n", vtop->c.ul & 0xff);
                        break;
                    case 2:
                        pr("pea.w %d\n", vtop->c.ul & 0xffff);
                        break;
                    case 4:
                        pr("pea.w %d\npea.w %d\n", vtop->c.ul >> 16, vtop->c.ul & 0xffff);
                        break;
                    default:
                        error("cannot push %d-byte immediate", length);
                        break;
                    }
                }
            } else {
                // load to register, then push
                pr("; ldpush before load type 0x%x reg 0x%x\n", vtop->type.t, vtop->r);
                r = gv(RC_INT);
                pr("; ldpush%d (type 0x%x reg 0x%x) tcc__r%d\n", length, vtop->type.t, vtop->r, r);
                switch (length) {
                case 1:
                    pr("sep #$20\nlda.b tcc__r%d\npha\nrep #$20\n", r);
                    break;
                // case 2: pr("lda.b tcc__r%d\npha\n", r); break;
                case 2:
                    pr("pei (tcc__r%d)\n", r);
                    break;
                case 4:
                    if ((vtop->type.t & VT_BTYPE) == VT_LLONG) {
                        // pr("lda.b tcc__r%d\npha\nlda.b tcc__r%d\npha\n", vtop->r2, r);
                        pr("pei (tcc__r%d)\npei (tcc__r%d)\n", vtop->r2, r);
                    }
                    // else pr("lda.b tcc__r%dh\npha\nlda.b tcc__r%d\npha\n", r, r);
                    else
                        pr("pei (tcc__r%dh)\npei (tcc__r%d)\n", r, r);
                    break;
                default:
                    error("cannot push %d-byte from register", length);
                    break;
                }
            }
            args_size += length;
        }
        vtop--;
    }
    save_regs(0); /* save used temporary registers */
    func_sym = vtop->type.ref;
    func_call = func_sym->r;

    (void) func_call;

    pr("; call r 0x%x\n", vtop->r);
    if (vtop->r & VT_LVAL) {
        // call a function pointer
        if ((vtop->r & VT_VALMASK) == VT_LLOCAL) {
            SValue v1;
            v1.type.t = VT_PTR;
            v1.r = VT_LOCAL | VT_LVAL;
            v1.c.ul = vtop->c.ul;
            load(9, &v1);
            // the 65816 is two stoopid to do a jsl [r10], so we have to jump thru a hoop here
            pr("; eins\njsr.l tcc__jsl_ind_r9\n");
        } else { // call a symbolic function pointer
            pr("; symfpcall vtop->sym %p vtop->r 0x%x vtop->type.t 0x%x c 0x%x\n",
               vtop->sym,
               vtop->r,
               vtop->type.t,
               vtop->c.ui);
            gv(RC_R10);
            pr("; zwei\njsr.l tcc__jsl_r10\n");
        }
    } else
        pr("jsr.l %s\n", get_sym_str(vtop->sym));

    if (args_size - restore_args_size && func_sym->r != FUNC_STDCALL) {
        pr("; add sp, #%d\n", args_size - restore_args_size);
        // pull the arguments off the stack
        if (args_size - restore_args_size == 2)
            pr("pla\n");
        else
            pr("tsa\nclc\nadc #%d\ntas\n", args_size - restore_args_size);
    }
    args_size = restore_args_size;
    /* the call may have reused the slots: this function's own fastcall
       parameters back from its frame copy */
    for (k = 0; k < fc_n; k++)
        pr("lda __%s_ps%d + %d + __%s_locals + 1,s\nsta.b tcc__a%d\n", current_fn, k, args_size, current_fn, k);
    vtop--;
}

/**
 * @brief Create a jump instruction.
 *
 * This function generates a jump instruction to a particular target address. It keeps track of all
 * generated jumps, and updates any jumps that were previously targeted at the same address as the
 * new jump.
 *
 * Each jump is assigned a unique index, which is used for subsequent operations on that jump.
 *
 * @param[in] t The target address for the jump.
 *
 * @return The unique index of the newly generated jump.
 */
int gjmp(int t)
{
    int r = ind;

    pr("; gjmp_addr %d at %d\n", t, ind);
    pr("jmp.w " LOCAL_LABEL "\n", jumps);

    ensure_jump_capacity();
    jump[jumps][0] = r;

    for (int i = 0; i < jumps; i++) {
        if (jump[i][0] == t) {
            jump[i][0] = r;
        }
    }

    jumps++;
    gsym_addr(r, t);

    return r;
}

/**
 * @brief Generates a jump instruction to a fixed address.
 *
 * This function is a wrapper around the `gjmp()` function. It calls `gjmp()` with the given address.
 *
 * @param[in] a The target address for the jump.
 *
 * @return void.
 */
void gjmp_addr(int a)
{
    gjmp(a);
}

/**
 * @brief Generates test instructions for a given condition.
 *
 * This function generates assembly code for various types of tests and comparisons.
 * It supports different types of values, including floats, constants, and jumps.
 * It also supports negation/inversion of the condition.
 *
 * @param[in] inv If non-zero, invert the condition.
 * @param[in] t The current position in the generated code (the instruction index).
 *
 * @return The index of the next instruction in the generated code. If a branch
 *         was generated, this could be the index of a jump target.
 */
/* Compares leave the CPU flags and a VT_CMP value (vtop->c.i = the
   compare token); the branch is made from the flags. cmp_keep_y asks a
   compare to keep the difference in Y as well (the high word of a 32-bit
   compare: gen_opl tests it after). */
int cmp_keep_y = 0;
static int cmp_labels = 0;

/* the branches that skip to __cs<n> unless cond holds; the caller then
   puts __ct<n>: and the jump taken when it holds, then __cs<n>:. After an
   unsigned compare (cmp, or sbc) C and Z; after a signed one (sbc) N, V
   and Z, with A the difference: N ^ V is "less than"
   (6502.org/tutorials/compare_beyond.html) */
static int jcc_begin(int cond)
{
    int n = cmp_labels++;
    switch (cond) {
    case TOK_EQ:
        pr("bne __cs%d\n", n);
        break;
    case TOK_NE:
        pr("beq __cs%d\n", n);
        break;
    case TOK_ULT:
        pr("bcs __cs%d\n", n);
        break;
    case TOK_UGE:
        pr("bcc __cs%d\n", n);
        break;
    case TOK_UGT:
        pr("beq __cs%d\nbcc __cs%d\n", n, n);
        break;
    case TOK_ULE:
        pr("bcc __ct%d\nbne __cs%d\n", n, n);
        break;
    case TOK_LT:
        pr("bvc __ca%d\neor #$8000\n__ca%d:\nbpl __cs%d\n", n, n, n);
        break;
    case TOK_GE:
        pr("bvc __ca%d\neor #$8000\n__ca%d:\nbmi __cs%d\n", n, n, n);
        break;
    case TOK_GT:
        pr("beq __cs%d\nbvc __ca%d\neor #$8000\n__ca%d:\nbmi __cs%d\n", n, n, n, n);
        break;
    case TOK_LE:
        pr("beq __ct%d\nbvc __ca%d\neor #$8000\n__ca%d:\nbpl __cs%d\n", n, n, n, n);
        break;
    default:
        error("unknown compare 0x%x", cond);
    }
    return n;
}

int gtst(int inv, int t)
{
    int v, r;
    v = vtop->r & VT_VALMASK;
    r = ind;
    pr("; gtst inv %d t %d v %d r %d ind %d\n", inv, t, v, r, ind);
    if (v == VT_CMP) {
        int n;
        pr("; cmp op 0x%x inv %d v %d r %d\n", vtop->c.i, inv, v, r);
        /* jump when the compare holds (inv: when it doesn't); chain t's
           jumps land on the brl and go on from there */
        ensure_jump_capacity();
        jump[jumps][0] = r;
        n = jcc_begin(inv ? (vtop->c.i ^ 1) : vtop->c.i);
        pr("__ct%d:\n", n);
        gsym(t);
        pr("brl " LOCAL_LABEL "\n__cs%d:\n", jumps++, n);
        t = r;
    } else if (v == VT_JMP || v == VT_JMPI) {
        pr("; VT_jmp r %d t %d ji %d inv %d vtop->c.i %d\n", r, t, v & 1, inv, vtop->c.i);
        if ((v & 1) == inv) {
            /* merge chain t into the value's chain: both jump where it
               will. (This was gsym(t), which sent t's jumps HERE, to the
               other outcome: `x != 0xFFFFFFFF && x >= y` took its true
               path when x was 0xFFFFFFFF.) A chain is every jump keyed
               with its id (gjmp re-keys the same way). */
            int i;
            if (t && vtop->c.i) {
                for (i = 0; i < jumps; i++)
                    if (jump[i][0] == t)
                        jump[i][0] = vtop->c.i;
                t = vtop->c.i;
            } else if (vtop->c.i)
                t = vtop->c.i;
        } else {
            t = gjmp(t);
            gsym(vtop->c.i);
        }
    } else {
        if (is_float(vtop->type.t)) {
            pr("; float 4\n");
            v = gv(RC_FLOAT);
            gsym(t);
            pr("lda.b tcc__f%d\nand.w #$ff00\nora.b tcc__f%dh\n", v - TREG_F0, v - TREG_F0);
            vtop->r = VT_CMP;
            vtop->c.i = TOK_NE;
            return gtst(inv, t);
        } else if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
            if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
                error("42 (Deep Thought)");
            if ((vtop->c.i != 0) != inv) {
                pr("; uncond jump: go! (vtop->c.i %d, inv %d)\n", vtop->c.i, inv);
                /* set flags as if we had a false compare result */
                pr("lda.w #0\n");
                t = gjmp(t);
            } else
                pr("; uncond jump: nop\n");
        } else {
            v = gv(RC_INT);
            gsym(t);
            pr("; tcc__r%d to compare reg\n", v);
            pr("lda.b tcc__r%d ; DON'T OPTIMIZE\n", v);
            if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
                pr("ora.b tcc__r%d\n", vtop->r2);
            vtop->r = VT_CMP;
            vtop->c.i = TOK_NE;
            return gtst(inv, t);
        }
    }
    vtop--;
    pr("; gtst finished; t %d\n", t);
    return t;
}

/**
 * @brief This function performs various arithmetic operations based on the input opcode.
 *
 * @param op The operation code that determines the arithmetic operation to be performed.
 *
 * The function performs operations such as addition, subtraction, multiplication, division,
 * bit shifting, and comparison based on the input op code. The operations are performed on
 * global stack top value `vtop` which is a global variable. The result is then stored back
 * into `vtop`.
 *
 * Some of the operations performed include:
 * - For multiplication (`*`), it optimizes for 8-bit computations.
 * - For unsigned multiplication (`TOK_UMULL`), it generates assembly for 32-bit multiplication.
 * - For division and modulus operations (`TOK_PDIV`, `/`, `TOK_UDIV`, `%`, `TOK_UMOD`), it handles both signed and unsigned division.
 * - For bitwise operations (`+`, `-`, `&`, `|`, `^`), it handles the carry flag accordingly.
 * - For comparison operations (`TOK_EQ`, `TOK_NE`, `TOK_GT`, `TOK_LE`, `TOK_LT`, `TOK_GE`, `TOK_UGT`, `TOK_ULE`, `TOK_ULT`, `TOK_UGE`), it handles signed and unsigned comparisons.
 * - For bitwise shift operations (`TOK_SAR`, `TOK_SHR`, `TOK_SHL`), it handles arithmetic and logical right shifts and left shifts.
 *
 * The function generates assembly code for these operations.
 */
void gen_opi(int op)
{
    int r, fr, fc, c, r5; // only set, remove it ,ft;
    char *opcrem = 0, *opcalc = 0, *opcarry = 0;
    int optone;
    int docarry;
    int sign;
    int div;
    int length, align;
    int isconst = 0;
    int timesshift, i;
    int skipcall;

    /* indexed globals: symbol + int register becomes a VT_IDXSYM value
       (the register holds the offset), and a constant added to one goes
       into its displacement, so arr[i] is ldx + lda.l arr,x and a struct
       field adds nothing at run time */
    if (op == '+' || op == '-') {
        SValue *a = &vtop[-1], *b = &vtop[0];
        if (is_idx_addr(a) && is_plain_const(b)) {
            a->c.i += (op == '+') ? b->c.i : -b->c.i;
            vtop--;
            return;
        }
        if (op == '+' && is_idx_addr(b) && is_plain_const(a)) {
            b->c.i += a->c.i;
            vtop[-1] = *b;
            vtop--;
            return;
        }
        if (op == '+' && ((is_sym_addr(a) && is_int_val(b)) || (is_sym_addr(b) && is_int_val(a)))) {
            Sym *sym;
            int disp, ix;
            if (is_sym_addr(a)) {
                sym = a->sym;
                disp = a->c.i;
                ix = gv(RC_INT);            /* the index, vtop */
            } else {
                sym = b->sym;
                disp = b->c.i;
                vswap();
                ix = gv(RC_INT);
                vswap();
            }
            vtop--;
            vtop->r = ix | VT_IDXSYM;
            vtop->r2 = VT_CONST;
            vtop->sym = sym;
            vtop->c.i = disp;
            return;
        }
    }

    length = type_size(&vtop[0].type, &align);
    r = vtop[-1].r;
    fr = vtop[0].r;
    fc = vtop[0].c.ul;

    // get the actual values
    if ((fr & VT_VALMASK) == VT_CONST && op != TOK_UMULL && !(fr & VT_SYM)) {
        // vtop is const, only need to load the other one
        // useless ? ft = vtop[0].type.t;
        vtop--;
        r = gv(RC_INT);
        isconst = 1;
        if (length <= 2 && (fc < -32768 || fc > 65535)) {
            warning("large integer implicitly truncated");
            fc = (short) fc;
        }
    } else {
        // have to load both operands to registers
        gv2(RC_INT, RC_INT);
        r = vtop[-1].r;
        fr = vtop[0].r;
        // useless ? ft = vtop[0].type.t;
        vtop--;
    }

    // remove non ascii char in comments for wla-dx
    if (op < 127)
        pr("; gen_opi len %d op %c\n", length, op);
    else
        pr("; gen_opi len %d op 0x%x\n", length, op);

    switch (op) {
    // multiplication
    case '*':
        skipcall = 0;
        if (isconst) {
            // optimize multiplication by constants using shifts and add/sub
            // SAFE patterns only: 2^n (shifts), 2^n+1 (shift+add), 2^n-1 (shift-sub)
            // NOTE: Composite patterns (6, 10, 12, etc.) NOT optimized because
            // they require temporary storage in tcc__r9 which may conflict with
            // other operations in complex expressions.
            switch (fc) {
            // powers of 2: just shifts
            case 2:
                skipcall = 1;
                pr("; mul #2 (optimized)\n");
                pr("lda.b tcc__r%d\nasl a\n", r);
                break;
            case 4:
                skipcall = 1;
                pr("; mul #4 (optimized)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\n", r);
                break;
            case 8:
                skipcall = 1;
                pr("; mul #8 (optimized)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\n", r);
                break;
            case 16:
                skipcall = 1;
                pr("; mul #16 (optimized)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\n", r);
                break;
            case 32:
                skipcall = 1;
                pr("; mul #32 (optimized)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\nasl a\n", r);
                break;
            case 64:
                skipcall = 1;
                pr("; mul #64 (optimized)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\nasl a\nasl a\n", r);
                break;
            case 128:
                skipcall = 1;
                pr("; mul #128 (optimized)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\nasl a\nasl a\nasl a\n", r);
                break;
            case 256:
                skipcall = 1;
                pr("; mul #256 (optimized)\n");
                pr("lda.b tcc__r%d\nxba\nand #$ff00\n", r);
                break;
            // 2^n + 1: shift then add original (no temp needed)
            case 3:  // 2 + 1
                skipcall = 1;
                pr("; mul #3 (optimized: x*2+x)\n");
                pr("lda.b tcc__r%d\nasl a\nclc\nadc.b tcc__r%d\n", r, r);
                break;
            case 5:  // 4 + 1
                skipcall = 1;
                pr("; mul #5 (optimized: x*4+x)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nclc\nadc.b tcc__r%d\n", r, r);
                break;
            case 9:  // 8 + 1
                skipcall = 1;
                pr("; mul #9 (optimized: x*8+x)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nclc\nadc.b tcc__r%d\n", r, r);
                break;
            case 17: // 16 + 1
                skipcall = 1;
                pr("; mul #17 (optimized: x*16+x)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\nclc\nadc.b tcc__r%d\n", r, r);
                break;
            case 33: // 32 + 1
                skipcall = 1;
                pr("; mul #33 (optimized: x*32+x)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\nasl a\nclc\nadc.b tcc__r%d\n", r, r);
                break;
            case 65: // 64 + 1
                skipcall = 1;
                pr("; mul #65 (optimized: x*64+x)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\nasl a\nasl a\nclc\nadc.b tcc__r%d\n", r, r);
                break;
            // 2^n - 1: shift then subtract original (no temp needed)
            case 7:  // 8 - 1
                skipcall = 1;
                pr("; mul #7 (optimized: x*8-x)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nsec\nsbc.b tcc__r%d\n", r, r);
                break;
            case 15: // 16 - 1
                skipcall = 1;
                pr("; mul #15 (optimized: x*16-x)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\nsec\nsbc.b tcc__r%d\n", r, r);
                break;
            case 31: // 32 - 1
                skipcall = 1;
                pr("; mul #31 (optimized: x*32-x)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\nasl a\nsec\nsbc.b tcc__r%d\n", r, r);
                break;
            case 63: // 64 - 1
                skipcall = 1;
                pr("; mul #63 (optimized: x*64-x)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\nasl a\nasl a\nsec\nsbc.b tcc__r%d\n", r, r);
                break;
            case 127: // 128 - 1
                skipcall = 1;
                pr("; mul #127 (optimized: x*128-x)\n");
                pr("lda.b tcc__r%d\nasl a\nasl a\nasl a\nasl a\nasl a\nasl a\nasl a\nsec\nsbc.b tcc__r%d\n", r, r);
                break;
            case 255: // 256 - 1
                skipcall = 1;
                pr("; mul #255 (optimized: x*256-x)\n");
                pr("lda.b tcc__r%d\nxba\nand #$ff00\nsec\nsbc.b tcc__r%d\n", r, r);
                break;
            default: {
                /* any other constant: its bits from the top down
                   (Horner), the running product in A and x added from its
                   own register, so no scratch register (why the composite
                   cases above were dropped). Mod 2^16, so signed is the
                   same. A negative constant: by -c, then negate. Used when
                   it beats tcc__mul (about 125 cycles with its set-up). */
                unsigned m = fc & 0xffff, neg = 0;
                int bits, adds = 0, b, cost;
                if ((m & 0x8000) && m != 0x8000) {
                    m = (0x10000 - m) & 0xffff;
                    neg = 1;
                }
                for (b = 0; b < 16; b++)
                    if (m & (1u << b))
                        adds++;
                for (bits = 16; bits > 0 && !(m & (1u << (bits - 1))); bits--)
                    ;
                cost = 4 + 2 * (bits - 1) + 5 * (adds - 1) + (neg ? 5 : 0);
                if (m == 0) {
                    skipcall = 1;
                    pr("; mul #0\nlda.w #0\n");
                } else if (tcc_state->hx422_mul && cost > 25) {
                    /* the HX-422 multiplier beats shifts and adds here:
                       about 25 cycles */
                    skipcall = 1;
                    pr("; mul #%d (HX-422 multiplier)\n", fc);
                    pr("lda.w #%d\nsta.l $003C30\nlda.b tcc__r%d\nsta.l $003C32\nlda.l $003C34\n", fc & 0xffff, r);
                } else if (cost < 90) {
                    skipcall = 1;
                    pr("; mul #%d (optimized: bits of %u, top down)\n", fc, m);
                    pr("lda.b tcc__r%d\n", r);
                    for (b = bits - 2; b >= 0; b--) {
                        pr("asl a\n");
                        if (m & (1u << b))
                            pr("clc\nadc.b tcc__r%d\n", r);
                    }
                    if (neg)
                        pr("eor.w #$ffff\ninc a\n");
                } else {
                    pr("; mul #%d, tcc__r%d\n", fc, r);
                    pr("lda.w #%d\nsta.b tcc__r9\n", fc);
                }
                break;
            }
            }
        } else if (tcc_state->hx422_mul) {
            /* the HX-422 cart's multiplier: write A and B, read the
               product (its low word is the same signed or not) */
            skipcall = 1;
            pr("; mul tcc__r%d,tcc__r%d (HX-422 multiplier)\n", fr, r);
            pr("lda.b tcc__r%d\nsta.l $003C30\nlda.b tcc__r%d\nsta.l $003C32\nlda.l $003C34\n", fr, r);
        } else {
            pr("; mul tcc__r%d,tcc__r%d\n", fr, r);
            pr("lda.b tcc__r%d\nsta.b tcc__r9\n", fr);
        }
        if (!skipcall) { // when no optim done, do the call
            pr("lda.b tcc__r%d\nsta.b tcc__r10\n", r);
            pr("jsr.l tcc__mul\n");
        }
        pr("sta.b tcc__r%d\n", r);
        break;

    case TOK_UMULL:
        r = vtop[0].r2 = get_reg(RC_INT);
        c = vtop[0].r;
        vtop[0].r = get_reg(RC_INT);
        pr("; umull tcc__r%d, tcc__r%d => tcc__r%d/tcc__r%d\n", c, vtop[1].r, vtop->r, r);
        if (tcc_state->hx422_mul) {
            /* the HX-422 multiplier's unsigned 32-bit product */
            pr("lda.b tcc__r%d\nsta.l $003C30\nlda.b tcc__r%d\nsta.l $003C32\n", c, vtop[1].r);
            pr("lda.l $003C34\nsta.b tcc__r%d\nlda.l $003C36\nsta.b tcc__r%d\n", vtop->r, r);
            break;
        }
        pr("lda.b tcc__r%d\nsta.b tcc__r9\nstz.b tcc__r9h\nlda.b tcc__r%d\nsta.b tcc__r10\nstz.b "
           "tcc__r10h\n",
           c,
           vtop[1].r);
        pr("jsr.l tcc__mull\n");
        pr("stx.b tcc__r%d\nsty.b tcc__r%d\n", r, vtop->r);
        break;
    // division and friends
    case TOK_PDIV:
        op = TOK_UDIV;
    case '/':
    case TOK_UDIV:
    case '%':
    case TOK_UMOD:
        if (op == '/' || op == '%')
            sign = 1;
        else
            sign = 0;
        if (op == '/' || op == TOK_UDIV)
            div = 1;
        else
            div = 0;

        // power-of-2 optimization for unsigned division/modulo
        // check if fc is a power of 2 (fc > 0 and only one bit set)
        if (isconst && !sign && fc > 0 && (fc & (fc - 1)) == 0) {
            int shift_count = 0;
            int temp_fc = fc;
            while (temp_fc > 1) {
                shift_count++;
                temp_fc >>= 1;
            }
            if (div) {
                // unsigned division by power of 2: use right shifts
                pr("; udiv #%d (power of 2), tcc__r%d => %d shifts\n", fc, r, shift_count);
                if (shift_count == 0) {
                    // division by 1, nothing to do
                } else if (shift_count <= 4) {
                    // unroll small shift counts
                    pr("lda.b tcc__r%d\n", r);
                    for (i = 0; i < shift_count; i++) {
                        pr("lsr a\n");
                    }
                    pr("sta.b tcc__r%d\n", r);
                } else if (shift_count == 8) {
                    // special case: swap bytes and mask
                    pr("lda.b tcc__r%d\nxba\nand #$00ff\nsta.b tcc__r%d\n", r, r);
                } else {
                    // use loop for larger shift counts
                    pr("lda.b tcc__r%d\nldy.w #%d\n-\nlsr a\ndey\nbne -\nsta.b tcc__r%d\n", r, shift_count, r);
                }
            } else {
                // unsigned modulo by power of 2: use AND with (fc - 1)
                pr("; umod #%d (power of 2), tcc__r%d => and #%d\n", fc, r, fc - 1);
                pr("lda.b tcc__r%d\nand #%d\nsta.b tcc__r%d\n", r, fc - 1, r);
            }
            break;
        }

        if (isconst) {
            pr("; div #%d, tcc__r%d\n", fc, r);
            pr("ldx.b tcc__r%d\n", r);
            pr("lda.w #%d\n", fc);
        } else {
            pr("; div tcc__r%d,tcc__r%d\n", fr, r);

            pr("ldx.b tcc__r%d\n", r);  // dividend to x
            pr("lda.b tcc__r%d\n", fr); // divisor to accu
        }
        pr("jsr.l tcc__%s\n", sign ? "div" : "udiv");

        if (div)
            pr("lda.b tcc__r9\nsta.b tcc__r%d\n", r); // quotient in r9...
        else
            pr("stx.b tcc__r%d\n", r); // ...remainder in x

        break;

    // intops the 65816 can do in hardware
    case '+':
    case TOK_ADDC1:
    case TOK_ADDC2:
    case '-':
    case TOK_SUBC1:
    case TOK_SUBC2:
    case '&':
    case TOK_LAND:
    case '|':
    case '^':
        optone = 1;
        docarry = 1;
        if (isconst && fc < 0) {
            if (op == '+') {
                op = '-';
                fc = -fc;
            } else if (op == '-') {
                op = '+';
                fc = -fc;
            } else if (op == TOK_ADDC1) {
                op = TOK_SUBC1;
                fc = -fc;
            } else if (op == TOK_SUBC1) {
                op = TOK_ADDC1;
                fc = -fc;
            } else if (op == TOK_ADDC2) {
                op = TOK_SUBC2;
                fc = -fc;
            } else if (op == TOK_SUBC2) {
                op = TOK_ADDC2;
                fc = -fc;
            }
        }
        if (op == '+' || op == TOK_ADDC1 || op == TOK_ADDC2) {
            opcalc = "adc";  // insn to use
            opcrem = "inc";  // *crement to use when doing this op with *crements
            opcarry = "clc"; // how to initialize the carry properly
            if (op == TOK_ADDC2)
                docarry = 0;
        } else if (op == '-' || op == TOK_SUBC1 || op == TOK_SUBC2) {
            opcalc = "sbc";
            opcrem = "dec";
            opcarry = "sec";
            if (op == TOK_SUBC2)
                docarry = 0;
        } else if (op == '&' || op == TOK_LAND) {
            optone = 0;  // optimize the "constant 1" operand case by using a *crement?
            docarry = 0; // touch the carry?
            opcalc = "and";
        } else if (op == '|') {
            optone = 0;
            docarry = 0;
            opcalc = "ora";
        } else if (op == '^') {
            optone = 0;
            docarry = 0;
            opcalc = "eor";
        } else
            error("ICE 42");

        pr("; %s tcc__r%d (0x%x), tcc__r%d (0x%x) (fr type 0x%x c %d r type 0x%x)\n",
           opcalc,
           fr,
           fr,
           r,
           r,
           vtop[0].type.t,
           vtop[0].c.ul,
           vtop[-1].type.t);
        if (isconst) {
            pr("; length xxy %d vtop->type 0x%x\n", type_size(&vtop->type, &align), vtop->type.t);
            if (length == 4) {
                /* probably pointer arithmetic... */
                pr("; assuming pointer arith\n");
                if ((fc >> 16) == 0)
                    pr("stz.b tcc__r%dh\n", r);
                else
                    pr("lda.w #%d\nsta.b tcc__r%dh\n", fc >> 16, r);
            }
            if (fc == 1 && optone)
                pr("%s.b tcc__r%d\n", opcrem, r);
            else if (fc == 2 && optone)
                pr("%s.b tcc__r%d\n%s.b tcc__r%d\n", opcrem, r, opcrem, r);
            else
                pr("%s\nlda.b tcc__r%d\n%s.w #%d\nsta.b tcc__r%d\n",
                   docarry ? opcarry : "; nop",
                   r,
                   opcalc,
                   fc & 0xffff,
                   r);
        } else {
            pr("; length xxy %d vtop->type 0x%x\n", type_size(&vtop->type, &align), vtop->type.t);
            pr("%s\nlda.b tcc__r%d\n%s.b tcc__r%d\nsta.b tcc__r%d\n",
               docarry ? opcarry : "; nop",
               r,
               opcalc,
               fr,
               r);
        }
        break;

    case TOK_EQ:
    case TOK_NE:
    case TOK_UGT:
    case TOK_ULE:
    case TOK_ULT:
    case TOK_UGE:
    case TOK_GT:
    case TOK_LE:
    case TOK_LT:
    case TOK_GE: {
        /* the flags and a VT_CMP: gtst branches on them, load turns them
           into 0 or 1 when the value is wanted. Signed compares need V,
           so sbc; the rest cmp, unless gen_opl wants the difference in Y */
        int is_signed = (op == TOK_GT || op == TOK_LE || op == TOK_LT || op == TOK_GE);
        char opnd[64];
        if (isconst)
            snprintf(opnd, sizeof opnd, ".w #%d", fc & 0xffff);
        else
            snprintf(opnd, sizeof opnd, ".b tcc__r%d", fr);
        pr("; cmp 0x%x tcc__r%d, %s\n", op, r, opnd);
        if (is_signed || cmp_keep_y)
            pr("lda.b tcc__r%d\nsec\nsbc%s\n", r, opnd);
        else
            pr("lda.b tcc__r%d\ncmp%s\n", r, opnd);
        if (cmp_keep_y)
            pr("tay\n");
        vtop->r = VT_CMP;
        vtop->c.i = op;
        break;
    }
    case TOK_SAR:
    case TOK_SHR:
    case TOK_SHL:
        timesshift = 1;
#define UNROLL_SHIFT_MAX 4
#define SHIFT_IN_PLACE_MAX 2
        if (isconst) {
            pr("; %s tcc__r%d, #%d\n",
               op == TOK_SAR   ? "sar"
               : op == TOK_SHR ? "shr"
                               : "shl",
               r,
               fc);
            if (!fc)
                return; // 0 -> nothing to do
            if (fc == 8 && (op == TOK_SHR || op == TOK_SHL)) {
                pr("lda.b tcc__r%d\nxba\n", r);
                switch (op) {
                case TOK_SHL:
                    pr("and #$ff00\n");
                    break;
                case TOK_SHR:
                    pr("and #$00ff\n");
                    break;
                default:
                    error("ICE 43");
                }
                pr("sta.b tcc__r%d\n", r);
                return;
            } else if (fc > UNROLL_SHIFT_MAX) // too many shifts -> need a loop
                pr("lda.b tcc__r%d\nldy.w #%d\n-\n", r, fc);
            else if (fc > SHIFT_IN_PLACE_MAX) {
                pr("lda.b tcc__r%d\n", r);
                timesshift = fc;
            } else {
                // very few shifts; don't bother loading the value to the accu
                for (i = 0; i < fc; i++) {
                    switch (op) {
                    case TOK_SAR:
                        pr("cmp #$8000\n"); // carry <= number negative?
                        pr("ror.b tcc__r%d\n", r);
                        break;
                    case TOK_SHR:
                        pr("lsr.b tcc__r%d\n", r);
                        break;
                    case TOK_SHL:
                        // for left shifts, signedness is irrelevant (in spite of what the mnemonic seems to suggest)
                        pr("asl.b tcc__r%d\n", r);
                        break;
                    default:
                        error("unknown shift");
                    }
                }
                return;
            }
        } else {
            pr("; %s tcc__r%d, tcc__r%d\n",
               op == TOK_SAR   ? "sar"
               : op == TOK_SHR ? "shr"
                               : "shl",
               r,
               fr);
            pr("lda.b tcc__r%d\nldy.b tcc__r%d\nbeq +\n-\n", r, fr);
        }
        for (i = 0; i < timesshift; i++)
            switch (op) {
            case TOK_SAR:
                pr("cmp #$8000\n"); // carry <= number negative?
                pr("ror a\n");
                break;
            case TOK_SHR:
                pr("lsr a\n");
                break;
            case TOK_SHL:
                // for left shifts, signedness is irrelevant (in spite of what the mnemonic seems to suggest)
                pr("asl a\n");
                break;
            default:
                error("unknown shift");
            }
        if (isconst && fc <= UNROLL_SHIFT_MAX)
            pr("sta.b tcc__r%d\n", r);
        else
            pr("dey\nbne -\n+\nsta.b tcc__r%d\n", r);
        break;
    default:
        error("opi 0x%x (%c) unimplemented\n", op, op);
    }
}

/**
 * @brief Converts a 32-bit floating-point number to a WOZ format.
 *
 * This function uses bitwise operations to transform a float into a WOZ format.
 * It first creates a bitwise representation of the float, and then
 * checks if the sign bit is set. If it is, the function sets the sign
 * bit and the exponent in the WOZ format and performs a two's complement
 * on the bitwise representation. The function then shifts the bits
 * of the bitwise representation to the right and stores the bytes
 * in the WOZ format.
 *
 * @param[in] f The 32-bit floating-point number to be converted.
 * @param[out] w Pointer to an array where the WOZ format will be stored.
 *
 * @note The WOZ format is stored in the array in big-endian format.
 */
void float_to_woz(float f, unsigned char *w)
{
    unsigned int i;
    unsigned char exp = 0x8e + 16; // 0x8e is the exp for 16-bit integers; we have 32-bit ints here

    memcpy(&i, &f, sizeof(f));
    if (i & 0x80000000UL) // check sign bit
    {
        w[0] = 0x80 | exp; // set sign bit and exp
        i = ~i + 1;        // 2's complement
    } else {
        w[0] = exp;
    }

    w[1] = (i >> 24) & 0xff;
    w[2] = (i >> 16) & 0xff;
    w[3] = (i >> 8) & 0xff;
}

/**
 * @brief Generate assembly code for floating point operations.
 *
 * This function generates assembly code based on a given operator.
 * The operator can be a basic arithmetic operation such as addition,
 * subtraction, multiplication, and division, or a comparison operation.
 * Depending on the operation, the function calls the relevant assembly
 * subroutine.
 *
 * @param[in] op Operator that determines the operation to perform.
 *
 * @note If an unimplemented operator is given, the function calls
 * an error routine.
 *
 * @warning This function modifies the vtop global variable.
 *
 * @note The function makes heavy use of the global vtop variable.
 * This variable should be properly initialized before calling this function.
 */
void gen_opf(int op)
{
    int ir, align;

    // get the actual values
    gv2(RC_F1, RC_F0);

    vtop--;

    pr("; gen_opf len %d op 0x%x ('%c')\n", type_size(&vtop[0].type, &align), op, op);

    switch (op) {
    case '*':
        pr("jsr.l tcc__fmul\n");
        break;

    case '/':
        pr("jsr.l tcc__fdiv\n");
        break;

    case '+':
        pr("jsr.l tcc__fadd\n");
        break;

    case '-':
        pr("jsr.l tcc__fsub\n");
        break;

    case TOK_EQ:
    case TOK_NE:
        ir = get_reg(RC_INT);
        pr("jsr.l tcc__fcmp\n");
        pr("dec a\n");
        pr(op == TOK_EQ ? "beq +\n" : "bne +\n");
        pr("dex\n+\nstx.b tcc__r%d\n", ir);
        vtop->r = ir;
        return;

    case TOK_GT:
    case TOK_LE:
    case TOK_LT:
    case TOK_GE:
        ir = get_reg(RC_INT);
        pr("jsr.l tcc__fcmp\n");
        pr("sec\nsbc.w #1\n");
        pr(op == TOK_GT ? "beq ++\n" : "beq +++\n");
        pr("bvc +\neor #$8000\n");

        switch (op) {
        case TOK_GT:
        case TOK_GE:
            pr("+ bpl +++\n");
            break;
        case TOK_LE:
        case TOK_LT:
            pr("+ bmi +++\n");
            break;
        default:
            error("don't know how to handle signed 0x%x\n", op);
        }
        pr("++\ndex\n+++\nstx.b tcc__r%d\n", ir);
        vtop->r = ir;
        return;
    default:
        error("opf 0x%x (%c) unimplemented\n", op, op);
    }
    vtop->r = TREG_F0;
}

/**
 * @brief Prints the function name based on the input type.
 *
 * The function checks if the input type is unsigned or not.
 * Based on the result, it prints the corresponding function name.
 *
 * @param[in] it The input type which will be checked if it is unsigned or not.
 * @param[in] unsigned_fn The function name to print if the input type is unsigned.
 * @param[in] signed_fn The function name to print if the input type is not unsigned (i.e., it's signed).
 *
 * @note This function does not call the printed functions. It merely selects
 * and prints the appropriate function name.
 */
void convert_type_helper(int it, const char *unsigned_fn, const char *signed_fn)
{
    if (it & VT_UNSIGNED)
        pr(unsigned_fn);
    else
        pr(signed_fn);
}

/**
 * @brief Generates code for converting an integer to a floating-point number.
 *
 * The function accepts an integer and generates appropriate machine code
 * to perform the conversion to a floating-point number. It uses the input
 * integer's type to determine how to perform the conversion.
 *
 * @param[in] t Not used directly in the function, but inferred to be the type
 * of the floating-point number to which the integer should be converted.
 *
 * @note This function does not return a value, but modifies the global
 * variable `vtop` to indicate that the result of the conversion is in
 * the `TREG_F0` register.
 */
void gen_cvt_itof(int t)
{
    int r, r2, it;
    gv(RC_INT);        // load integer to convert
    r = vtop->r;       // register with int
    r2 = vtop->r2;     // register with high word (for long longs)
    it = vtop->type.t; // type of int
    pr("; itof tcc__r%d, f0\n", r);
    if ((vtop->type.t & VT_BTYPE) == VT_LLONG) {
        pr("pei (tcc__r%d)\npei (tcc__r%d)\n", r2, r);
        convert_type_helper(it, "jsr.l tcc__ulltof\n", "jsr.l tcc__lltof\n");
        pr("pla\npla\n");
    } else {
        get_reg(RC_F0); // result will go to f0
        pr("lda.b tcc__r%d\n", r);
        pr("xba\nsta.b tcc__f0 + 1\n"); // convert to big-endian and load to upper 2 bytes of mantissa
        convert_type_helper(it, "jsr.l tcc__ufloat\n", "jsr.l tcc__float\n");
    }
    vtop->r = TREG_F0; // tell TCC that the result is in f0
}

/**
 * @brief Generates code for converting a floating-point number to an integer.
 *
 * The function accepts a floating-point number and generates the appropriate
 * machine code to perform the conversion to an integer. It uses the target
 * integer's type to determine how to perform the conversion.
 *
 * @param[in] t The type of the integer to which the floating-point number
 * should be converted.
 *
 * @note This function does not return a value, but modifies the global
 * variable `vtop` to indicate that the result of the conversion is in
 * the specified register.
 */
void gen_cvt_ftoi(int t)
{
    int r = 0;
    gv(RC_F0);
    int is_llong = (t & VT_BTYPE) == VT_LLONG;
    if (is_llong) {
        get_reg(RC_R0);
        get_reg(RC_R1);
    } else {
        r = get_reg(RC_INT);
    }

    pr("; ftoi tcc__f0, tcc__r%d(type 0x%x)\n", r, t);
    pr("lda #%d\nsta.b tcc__r9\n", (t & VT_UNSIGNED) ? 0 : 1);

    if (is_llong || (t & VT_UNSIGNED)) {
        pr("jsr.l tcc__llfix\n");
        vtop->r2 = TREG_R1;
        vtop->r = TREG_R0;
    } else {
        pr("jsr.l tcc__fix\n");
        pr("lda.b tcc__f0 + 1\nxba\nsta.b tcc__r%d\n", r);
        vtop->r = r;
    }
}

/**
 * @brief Throws an error for unsupported conversion between floating point types.
 *
 * This function is intended to handle conversion from one floating-point type
 * to another. However, in the current implementation, it throws an error
 * message, as such conversion is not supported.
 *
 * @param[in] t The target type of the floating-point conversion.
 *
 * @note This function does not perform any actual operations; it always results
 * in an error. If your project requires floating-point to floating-point conversions,
 * this function needs to be implemented accordingly.
 */
void gen_cvt_ftof(int t)
{
    error("gen_cvt_ftof 0x%x\n", t);
}

/**
 * @brief Performs an unconditional goto (jump) to a specified address.
 *
 * This function is used to implement an unconditional jump to a target address.
 * The target address is obtained from the integer register specified by `r`.
 * The function assumes that the target address is in the same format as the
 * program counter address, based on the provided `type` information.
 *
 * @note The target address is typically obtained from a register or a constant.
 *
 * @param[in] r The register containing the target address.
 * @param[in] t The type of the target address, providing information about the format
 *              and representation of the address.
 *
 * @note This function does not have a return value as it performs a direct jump
 * to the specified address.
 */
void ggoto(void)
{
    int r = gv(RC_INT);
    int t = vtop->type.t;
    pr("; ggoto r 0x%x t 0x%x\n", r, t);
    pr("lda.b tcc__r%d\nsta.b tcc__r9 + 1\nsep #$20\nlda.b tcc__r%dh\nsta.b tcc__r9h + 1\nlda.b "
       "#$5c\nsta.b tcc__r9\nrep #$20\n",
       r,
       r);
    pr("jml.l tcc__r9\n");
}

int ind_before_section = 0;
int section_closed = 1;
int section_count = 0;

/**
 * @brief Generates the function prolog for a given function type.
 *
 * This function is responsible for generating the prolog of a function based on
 * the provided function type. The prolog sets up the function's local variables,
 * initializes necessary registers, and prepares the function's stack frame.
 *
 * @param[in] func_type Pointer to the function type information.
 *
 * @note The function relies on various internal state variables and context to generate
 * the function prolog correctly. Please ensure that the necessary state is properly
 * set up before calling this function.
 */
void gfunc_prolog(CType *func_type)
{
    Sym *sym;
    int n, addr, size, align;

    sym = func_type->ref;
    func_vt = sym->type;

    n = 0;
    addr = 3; // skip 24-bit return address
    loc = 0;
    if ((func_vt.t & VT_BTYPE) == VT_STRUCT) {
        func_vc = addr;
        addr += PTR_SIZE;
        n += PTR_SIZE;
    }

    /* super-dirty hack to get the function name */
    strncpy(current_fn, get_sym_str((Sym *) (((void *) func_type) - offsetof(Sym, type))), MAXLEN - 1);
    current_fn[MAXLEN - 1] = '\0';

    /* wlalink does not cut up sections, so it is desirable to have a section
       for each function to keep the amount of unused memory in the ROM banks
       low. WLA DX barfs, however, if fed more than 255 sections, so we have
       to be a bit economical: gfunc_epilog() only closes the section if more
       than 50K of assembler code have been written */
    if (section_closed) {
        ind_before_section = ind;
        //230625 pr("\n.SECTION \".text_0x%x\" SUPERFREE\n", section_count++);
        pr("\n.SECTION \".%stext_0x%x\" SUPERFREE\n", current_fn, section_count++);
        section_closed = 0;
    }

    pr("\n%s:\n", current_fn);

    {
        int ns = fc_nslots(FUNC_CALL(func_type->ref->r)), k;
        fc_n = 0;
        fc_calls = 0;
        fc_args_ctx = 0;
        while ((sym = sym->next)) {
            CType *type = &sym->type;
            if (fc_n < ns && fc_fits(type)) {   /* in a slot, not on the stack */
                sym_push(sym->v & ~SYM_FIELD, type, VT_LOCAL | VT_LVAL, FC_SLOT + 2 * fc_n);
                fc_n++;
                continue;
            }
            sym_push(sym->v & ~SYM_FIELD, type, VT_LOCAL | VT_LVAL, addr);
            size = type_size(type, &align);
            addr += size;
            n += size;
        }
        pr("; sub sp,#__%s_locals\n", current_fn);
        pr(".ifgr __%s_locals 0\ntsa\nsec\nsbc #__%s_locals\ntas\n.endif\n", current_fn, current_fn);
        if (fc_n) {
            /* the slots to the frame, if the function makes calls */
            pr(".ifgr __%s_psave 0\n", current_fn);
            for (k = 0; k < fc_n; k++)
                pr("lda.b tcc__a%d\nsta __%s_ps%d + __%s_locals + 1,s\n", k, current_fn, k, current_fn);
            pr(".endif\n");
        }
        /* the direct page locals' slots it uses, saved (the prescan ran) */
        for (k = 0; k < dp_ncand; k++)
            pr(".ifgr __%s_dpu%d 0\nlda.b tcc__l%d\nsta __%s_dps%d + __%s_locals + 1,s\n.endif\n",
               current_fn, k, k, current_fn, k, current_fn);
    }
    loc = 0; // huh squared?
}

#define STACK_SIZE_LIMIT 0x1f00

// Dynamic locals arrays (replaces static char locals[MAX_LOCALS][MAXLEN] and int localnos[MAX_LOCALS])
char (*locals)[MAXLEN] = NULL;
int *localnos = NULL;
int localno = 0;
int locals_capacity = 0;

/** @brief Ensures the locals arrays have enough capacity */
static void ensure_locals_capacity(void)
{
    if (localno >= locals_capacity) {
        locals_capacity = locals_capacity ? locals_capacity * 2 : INITIAL_LOCALS_CAPACITY;
        locals = realloc(locals, locals_capacity * sizeof(*locals));
        localnos = realloc(localnos, locals_capacity * sizeof(*localnos));
        if (!locals || !localnos)
            error("out of memory allocating locals");
    }
}

/**
 * @brief Generates the function epilog.
 *
 * This function is responsible for generating the epilog of a function. The epilog
 * performs necessary cleanup operations, restores the stack pointer, and returns
 * from the function.
 *
 * @note The function relies on various internal state variables and context to generate
 * the function epilog correctly. Please ensure that the necessary state is properly set up
 * before calling this function.
 */
void gfunc_epilog(void)
{
    int k;
    /* the direct page locals' slots back as the caller had them */
    for (k = 0; k < dp_ncand; k++)
        pr(".ifgr __%s_dpu%d 0\nlda __%s_dps%d + __%s_locals + 1,s\nsta.b tcc__l%d\n.endif\n",
           current_fn, k, current_fn, k, current_fn, k);
    pr("; add sp, #__%s_locals\n", current_fn);
    pr(".ifgr __%s_locals 0\ntsa\nclc\nadc #__%s_locals\ntas\n.endif\n", current_fn, current_fn);
    {
        /* what comes back, for 816-opt's liveness (a trailing comment, which
           it keeps): void nothing, 16 bits r0, a pointer r0 and its bank
           r0h, 32 bits r0 and r1, a float f0; a struct (by a hidden
           pointer) left as it was, everything */
        int bt = func_vt.t & VT_BTYPE;
        const char *ret = "";
        if (bt == VT_VOID)
            ret = " ; retvoid";
        else if (is_float(func_vt.t))
            ret = " ; retf";
        else if (bt == VT_LLONG)
            ret = " ; ret32";
        else if (bt == VT_PTR || bt == VT_FUNC)
            ret = " ; retptr";
        else if (bt != VT_STRUCT)
            ret = " ; ret16";
        pr("rtl%s\n", ret);
    }

    pr(".ENDS\n");
    section_closed = 1;

    /* the direct page locals: frame room to save each slot used */
    for (k = 0; k < dp_ncand; k++) {
        if (dp_used[k]) {
            loc -= 2;
            fc_add_def(".define __%s_dpu%d 1", current_fn, k);
            fc_add_def(".define __%s_dps%d %d", current_fn, k, loc);
        } else {
            fc_add_def(".define __%s_dpu%d 0", current_fn, k);
            fc_add_def(".define __%s_dps%d 0", current_fn, k);
        }
    }
    dp_ncand = 0;

    /* fastcall parameters: a frame copy if the function made calls */
    if (fc_n) {
        fc_add_def(".define __%s_psave %d", current_fn, fc_calls ? 1 : 0);
        for (k = 0; k < fc_n; k++) {
            if (fc_calls) {
                loc -= 2;
                fc_add_def(".define __%s_ps%d %d", current_fn, k, loc);
            } else
                fc_add_def(".define __%s_ps%d 0", current_fn, k);
        }
        fc_n = 0;
    }

    if (-loc > STACK_SIZE_LIMIT) {
        error("stack overflow");
    }

    /* simply putting a ".define __<current_fn>_locals -<loc>" after the
       function does not work in some cases for unknown reasons (wla-dx
       complains about unresolved symbols); putting them before the reference
       works, but this has to be done by the output code, so we have to save
       the various locals sizes somewhere */
    ensure_locals_capacity();
    strncpy(locals[localno], current_fn, MAXLEN - 1);
    locals[localno][MAXLEN - 1] = '\0';
    localnos[localno] = -loc;
    localno++;

    current_fn[0] = '\0';
}
