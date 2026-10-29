/* __asmraw__'s text, read from the source as it is, the '(' just
   tokenised: into cstr, trimmed, with its NUL; the token after the ')' is
   tokenised */
static void asmraw_scan(CString *out)
{
    int has_quote = 0;
    int paren_depth = 0; /* tracks nested '(' in the unquoted case */
#define cstr (*out)

    /* IMPORTANT: next_nomacro1() only updates file->buf_ptr internally
       (via its local cursor 'p'); it never touches the global 'ch'
       variable. Since the code below reads raw source text directly
       through ch/inp() instead of going through the normal tokenizer,
       we must manually resync ch with the real buffer position here -
       otherwise ch would still hold a stale value left over from
       whatever the tokenizer last did (e.g. scanning the __asmraw__
       identifier), causing the raw scan below to start one character
       too early. */
    ch = file->buf_ptr[0];

    /* Skip whitespace after the opening parenthesis */
    while (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
        if (ch == '\n')
            file->line_num++;
        inp();
    }

    /* Optional quoted form: __asmraw__ ("...") */
    if (ch == '"') {
        has_quote = 1;
        inp();
    }

    cstr_new(&cstr);

    while (ch != CH_EOF) {
        if (has_quote && ch == '"') {
            break;
        }
        if (!has_quote) {
            if (ch == '(') {
                paren_depth++;
            } else if (ch == ')') {
                if (paren_depth == 0) {
                    break;
                }
                paren_depth--;
            }
        }

        if (ch == '\r') {
            inp();
            continue;
        }
        if (ch == '\n') {
            file->line_num++;
            cstr_ccat(&cstr, '\n');
            inp();
            continue;
        }

        cstr_ccat(&cstr, ch);
        inp();
    }

    cstr_ccat(&cstr, '\0');

    /* Trim trailing whitespace, newlines, and leftover quotes */
    char *p = (char *) cstr.data;
    int len = cstr.size - 1;
    while (len > 0
           && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r' || p[len - 1] == '\n'
               || p[len - 1] == '"')) {
        p[--len] = '\0';
    }

    /* Consume the closing quote, if any */
    if (has_quote && ch == '"') {
        inp();
    }

    /* Skip any trailing junk up to the closing ')' */
    while (ch != ')' && ch != CH_EOF) {
        if (ch == '\n')
            file->line_num++;
        inp();
    }

    if (ch == ')') {
        inp();
    }
    next();
#undef cstr
}

/* the text, as it is. NOTE: pr()'s internal buffer (char line[MAXLEN],
   512 bytes) would silently truncate long __asmraw__ blocks, so it is
   written directly */
static void asmraw_emit(const char *p)
{
    if (*p) {
        s("; asm {\n");          /* 816-opt leaves it as written */
        s((char *) p);
        s("\n; asm }\n");
    }
}

static void asmraw_instr(void)
{
    CString cstr;

    next();
    if (tok != '(') {
        expect("(");
    }
    if (macro_ptr) {
        /* from saved tokens (816: each function is compiled from them,
           twice), where the text is one string, read from the source when
           they were saved (gen_function); or a macro's quoted form */
        next();
        if (tok != TOK_STR)
            expect("__asmraw__'s text");
        asmraw_emit((char *) tokc.cstr->data);
        next();
        skip(')');
        return;
    }
    asmraw_scan(&cstr);
    asmraw_emit((char *) cstr.data);
    cstr_free(&cstr);
}

/* GCC-style asm() for the 65816 (the 816-tcc fork).

     asm [volatile] ("template" [: outputs [: inputs [: clobbers]]]);

   An operand is ["[name]"] "constraint" (C expression). Constraints:
     r   in a pseudo-register: %N is tcc__rK (%HN its high word, tcc__rKh,
         for a pointer)
     a   in A, x in X, y in Y: loaded before the template (an input),
         stored after it (an output); %N still names the pseudo-register
     i n a constant: %N is the number, or "sym + offset" for an address
     m   a global's memory: %N is "sym + offset" (write lda.l %N); a
         local is r (whether it's in a direct page slot is the dry run's
         call, so m can't depend on it)
     0-9 (inputs) in the same register as that output
   Outputs start with = (written) or + (read and written). Clobbers: "a"
   "cc" "memory" (nothing to do: no value lives in A or the flags across a
   statement, nor in a register across memory), "x" and "y" (keeps a
   loop's index or counter out of them). %% is %, %= a number unique to the
   statement (for labels: lbl%=). With no ':' the template is copied as it
   is, % and all, as GCC does.

   The block goes out between "; asm {" and "; asm }": the fork's 816-opt
   lifts it out before optimising and puts it back as written. The code
   in it must leave A, X and Y 16 bits wide, and D, DB and S as it found
   them. */
#define ASM816_MAX 10
typedef struct {
    int out, plus, match, kind; /* kind: r a x y i m */
    int vs;                     /* its value's (or lvalue's) place on the stack */
    int reg;                    /* the pseudo-register it's in, or -1 */
    int dummy;                  /* an output's: the stack place marking its register used */
    char name[32];
} asm816_op;

static int asm816_label;

void yl_asm_clobbers(int x, int y);

static void asm816_str(CString *s)
{
    if (tok != TOK_STR)
        expect("string constant");
    while (tok == TOK_STR) {
        cstr_cat(s, tokc.cstr->data);
        next();
    }
}

static int asm816_plain_reg(SValue *v)
{
    return !(v->r & (VT_LVAL | VT_IDXSYM)) && (v->r & VT_VALMASK) <= TREG_R5;
}

static int asm816_operands(asm816_op *ops, int n, int is_out)
{
    while (tok != ':' && tok != ')') {
        asm816_op *o;
        CString c;
        const char *p;
        int size, align;
        if (n >= ASM816_MAX)
            error("asm: more than %d operands", ASM816_MAX);
        o = &ops[n];
        memset(o, 0, sizeof *o);
        o->out = is_out;
        o->match = -1;
        o->reg = -1;
        o->dummy = -1;
        if (tok == '[') {
            next();
            if (tok < TOK_IDENT)
                expect("operand name");
            pstrcpy(o->name, sizeof o->name, get_tok_str(tok, NULL));
            next();
            skip(']');
        }
        cstr_new(&c);
        asm816_str(&c);
        cstr_ccat(&c, '\0');
        p = (char *) c.data;
        while (*p == '=' || *p == '+' || *p == '&') {
            if (*p == '+')
                o->plus = 1;
            p++;
        }
        if (is_out && ((char *) c.data)[0] != '=' && !o->plus)
            error("asm: an output's constraint begins with = or +");
        if (!is_out && *p >= '0' && *p <= '9') {
            o->match = *p - '0';
            o->kind = 'r';
        } else if (*p == 'r' || *p == 'a' || *p == 'x' || *p == 'y' || *p == 'm')
            o->kind = *p;
        else if (*p == 'i' || *p == 'n')
            o->kind = 'i';
        else
            error("asm: unknown constraint \"%s\" (r a x y i n m, or an output's number)", (char *) c.data);
        cstr_free(&c);
        if (is_out && o->kind == 'i')
            error("asm: an output can't be a constant");

        skip('(');
        gexpr(NULL);
        skip(')');
        size = type_size(&vtop->type, &align);
        if ((vtop->type.t & VT_BTYPE) == VT_STRUCT || (vtop->type.t & VT_BTYPE) == VT_LLONG
            || is_float(vtop->type.t) || size > 4 || (size > 2 && (vtop->type.t & VT_BTYPE) != VT_PTR))
            error("asm: an operand is 1 or 2 bytes, or a pointer");
        if (size > 2 && (o->kind == 'a' || o->kind == 'x' || o->kind == 'y'))
            error("asm: A, X and Y hold 2 bytes; use r for a pointer");
        if (o->kind == 'm') {
            if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != (VT_CONST | VT_LVAL | VT_SYM))
                error("asm: an m operand is a global (for a local, use r)");
        } else if (is_out) {
            test_lvalue();
        } else if (o->kind == 'i') {
            if ((vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST)
                error("asm: an i operand is a constant or an address");
        } else
            gv(RC_INT);         /* in a register now; checked again below */
        o->vs = vtop - vstack;
        n++;
        if (tok != ',')
            break;
        next();
    }
    return n;
}

/* the operand %... names, at p (past the %): its text into out */
static const char *asm816_subst(CString *out, const char *p, asm816_op *ops, int n)
{
    char buf[128];
    int k = -1, hi = 0;
    SValue *v;
    if (*p == 'H') {
        hi = 1;
        p++;
    }
    if (*p == '[') {
        const char *e = strchr(p, ']');
        int len;
        if (!e)
            error("asm: %%[ without ]");
        len = e - p - 1;
        for (k = 0; k < n; k++)
            if ((int) strlen(ops[k].name) == len && !strncmp(ops[k].name, p + 1, len))
                break;
        if (k == n)
            error("asm: no operand named %.*s", len, p + 1);
        p = e + 1;
    } else if (*p >= '0' && *p <= '9') {
        k = 0;
        while (*p >= '0' && *p <= '9')
            k = k * 10 + (*p++ - '0');
        if (k >= n)
            error("asm: operand %%%d, but there are %d", k, n);
    } else
        error("asm: %% followed by '%c' (%%N %%HN %%[name] %%%% %%=)", *p ? *p : ' ');
    v = &vstack[ops[k].vs];
    if (ops[k].kind == 'i') {
        if (hi)
            error("asm: %%H on a constant");
        if (v->r & VT_SYM)
            snprintf(buf, sizeof buf, "%s + %d", get_sym_str(v->sym), (int) v->c.i);
        else
            snprintf(buf, sizeof buf, "%d", (int) v->c.i);
    } else if (ops[k].kind == 'm') {
        snprintf(buf, sizeof buf, "%s + %d", get_sym_str(v->sym), (int) v->c.i + 2 * hi);
    } else
        snprintf(buf, sizeof buf, "tcc__r%d%s", ops[k].reg, hi ? "h" : "");
    cstr_cat(out, buf);
    return p;
}

static void asm816_instr(void)
{
    asm816_op ops[ASM816_MAX];
    int nout = 0, n = 0, i, clob_x = 0, clob_y = 0, operands = 0, first;
    CString tpl, out;
    const char *p;
    char buf[64];

    next();
    while (tok == TOK_VOLATILE1 || tok == TOK_VOLATILE2 || tok == TOK_VOLATILE3)
        next();
    skip('(');
    cstr_new(&tpl);
    asm816_str(&tpl);
    cstr_ccat(&tpl, '\0');
    first = vtop - vstack + 1;
    if (tok == ':') {
        operands = 1;
        next();
        n = nout = asm816_operands(ops, 0, 1);
        if (tok == ':') {
            next();
            n = asm816_operands(ops, n, 0);
            if (tok == ':') {
                next();
                while (tok == TOK_STR) {
                    CString c;
                    const char *cl;
                    cstr_new(&c);
                    asm816_str(&c);
                    cstr_ccat(&c, '\0');
                    cl = (char *) c.data;
                    if (!strcmp(cl, "x"))
                        clob_x = 1;
                    else if (!strcmp(cl, "y"))
                        clob_y = 1;
                    else if (strcmp(cl, "a") && strcmp(cl, "cc") && strcmp(cl, "memory"))
                        error("asm: unknown clobber \"%s\" (a x y cc memory)", cl);
                    cstr_free(&c);
                    if (tok != ',')
                        break;
                    next();
                }
            }
        }
    }
    skip(')');
    skip(';');

    /* outputs: a register each, kept marked on the stack until stored */
    for (i = 0; i < nout; i++) {
        if (ops[i].kind == 'm')
            continue;
        ops[i].reg = get_reg(RC_INT);
        vset(&vstack[ops[i].vs].type, ops[i].reg, 0);
        ops[i].dummy = vtop - vstack;
        if (ops[i].plus)
            load(ops[i].reg, &vstack[ops[i].vs]);
    }
    /* inputs: in their registers (an earlier one may have been spilled) */
    for (i = nout; i < n; i++) {
        SValue *v = &vstack[ops[i].vs];
        if (ops[i].kind == 'i' || ops[i].kind == 'm')
            continue;
        if (ops[i].match >= 0) {
            if (ops[i].match >= nout || ops[ops[i].match].reg < 0)
                error("asm: input %d matches no register output", i);
            ops[i].reg = ops[ops[i].match].reg;
            load(ops[i].reg, v);
            continue;
        }
        if (!asm816_plain_reg(v)) {
            int r = get_reg(RC_INT);
            load(r, v);
            v->r = r;
        }
        ops[i].reg = v->r & VT_VALMASK;
    }
    for (i = 0; i < n; i++)
        if ((ops[i].dummy >= 0 && (vstack[ops[i].dummy].r & VT_VALMASK) != ops[i].reg)
            || (i >= nout && ops[i].match < 0 && ops[i].reg >= 0
                && (!asm816_plain_reg(&vstack[ops[i].vs]) || (vstack[ops[i].vs].r & VT_VALMASK) != ops[i].reg)))
            error("asm: the operands need more registers than there are (6)");
    if (dry_run && (clob_x || clob_y))
        yl_asm_clobbers(clob_x, clob_y);

    /* the block */
    asm816_label++;
    cstr_new(&out);
    cstr_cat(&out, "; asm {\n");
    for (i = nout; i < n; i++)
        if (ops[i].kind == 'a' || ops[i].kind == 'x' || ops[i].kind == 'y') {
            snprintf(buf, sizeof buf, "ld%c.b tcc__r%d\n", ops[i].kind, ops[i].reg);
            cstr_cat(&out, buf);
        }
    for (p = (char *) tpl.data; *p;) {
        if (*p != '%' || !operands) {
            cstr_ccat(&out, *p++);
            continue;
        }
        p++;
        if (*p == '%') {
            cstr_ccat(&out, '%');
            p++;
        } else if (*p == '=') {
            snprintf(buf, sizeof buf, "%d", asm816_label);
            cstr_cat(&out, buf);
            p++;
        } else
            p = asm816_subst(&out, p, ops, n);
    }
    cstr_ccat(&out, '\n');
    for (i = 0; i < nout; i++)
        if (ops[i].kind == 'a' || ops[i].kind == 'x' || ops[i].kind == 'y') {
            snprintf(buf, sizeof buf, "st%c.b tcc__r%d\n", ops[i].kind, ops[i].reg);
            cstr_cat(&out, buf);
        }
    cstr_cat(&out, "; asm }\n");
    cstr_ccat(&out, '\0');
    s((char *) out.data);
    cstr_free(&out);
    cstr_free(&tpl);

    /* outputs into their C lvalues */
    for (i = 0; i < nout; i++)
        if (ops[i].reg >= 0)
            store(ops[i].reg, &vstack[ops[i].vs]);
    while (vtop >= vstack + first)
        vpop();
}

/* asm("...") outside a function: its text, in a section of its own (no
   operands) */
static void asm816_global_instr(void)
{
    CString tpl;
    next();
    skip('(');
    cstr_new(&tpl);
    asm816_str(&tpl);
    cstr_ccat(&tpl, '\0');
    skip(')');
    skip(';');
    if (!section_closed)
        error("asm: inside a function's section");
    /* at file scope: the text section, where functions go (as gen_function) */
    cur_text_section = text_section;
    ind = cur_text_section->data_offset;
    pr("\n.SECTION \".tcc_asm_0x%x\" SUPERFREE\n", section_count++);
    s("; asm {\n");
    s((char *) tpl.data);
    s("\n; asm }\n");
    pr(".ENDS\n");
    cur_text_section->data_offset = ind;
    cstr_free(&tpl);
}
