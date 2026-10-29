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
