/*
 * lexer.c -- Cando source lexer implementation.
 *
 * Implements the CandoLexer API declared in lexer.h.
 *
 * Design notes:
 *   - Single-pass, character-by-character scanner.
 *   - All tokens store a pointer+length into the original source buffer;
 *     no memory is allocated during lexing.
 *   - Keywords are detected after scanning an identifier via
 *     cando_keyword_type().
 *   - Multi-character operators are handled with explicit lookahead.
 *   - String literals: double-quote (""), single-quote (''), and backtick
 *     (``) are scanned as a single raw token inclusive of delimiters.
 *     Escape handling and backtick interpolation are left to the parser.
 *   - Comments: // line comments and slash-star block comments are skipped.
 */

#include "lexer.h"
#include <stdio.h>   /* snprintf */
#include <ctype.h>   /* isdigit, isalpha, isspace */
#include <string.h>  /* strlen, strncmp */

/* =========================================================================
 * token.c functions -- token type names and keyword table
 * ======================================================================= */

/* Maps every CandoTokenType to a short printable name. */
static const char *const TOKEN_TYPE_NAMES[TOK_COUNT] = {
    [TOK_NUMBER]         = "NUMBER",
    [TOK_STRING_DQ]      = "STRING_DQ",
    [TOK_STRING_SQ]      = "STRING_SQ",
    [TOK_STRING_BT]      = "STRING_BT",
    [TOK_IDENT]          = "IDENT",

    [TOK_IF]             = "IF",
    [TOK_ELSE]           = "ELSE",
    [TOK_WHILE]          = "WHILE",
    [TOK_FOR]            = "FOR",
    [TOK_FUNCTION]       = "FUNCTION",
    [TOK_CLASS]          = "CLASS",
    [TOK_EXTENDS]        = "EXTENDS",
    [TOK_RETURN]         = "RETURN",
    [TOK_THROW]          = "THROW",
    [TOK_TRY]            = "TRY",
    [TOK_CATCH]          = "CATCH",
    [TOK_FINALY]         = "FINALY",
    [TOK_CONST]          = "CONST",
    [TOK_VAR]            = "VAR",
    [TOK_GLOBAL]         = "GLOBAL",
    [TOK_STATIC]         = "STATIC",
    [TOK_PRIVATE]        = "PRIVATE",
    [TOK_ASYNC]          = "ASYNC",
    [TOK_AWAIT]          = "AWAIT",
    [TOK_THREAD]         = "THREAD",
    [TOK_NULL_KW]        = "NULL",
    [TOK_TRUE_KW]        = "TRUE",
    [TOK_FALSE_KW]       = "FALSE",
    [TOK_IN]             = "IN",
    [TOK_OF]             = "OF",
    [TOK_OVER]           = "OVER",
    [TOK_CONTINUE]       = "CONTINUE",
    [TOK_BREAK]          = "BREAK",
    [TOK_ALSO]           = "ALSO",
    [TOK_SETTLE]         = "SETTLE",
    [TOK_PIPE_KW]        = "pipe",

    [TOK_PIPE_OP]        = "~>",
    [TOK_FILTER_OP]      = "~!>",
    [TOK_COND_FILTER_OP] = "~&>",
    [TOK_QDOT]           = "?.",
    [TOK_QLBRACKET]      = "?[",
    [TOK_RANGE_ASC]      = "->",
    [TOK_RANGE_DESC]     = "<-",
    [TOK_FLUENT]         = "::",
    [TOK_VARARG]         = "...",
    [TOK_FAT_ARROW]      = "=>",
    [TOK_AND]            = "&&",
    [TOK_OR]             = "||",
    [TOK_EQ]             = "==",
    [TOK_NEQ]            = "!=",
    [TOK_LEQ]            = "<=",
    [TOK_GEQ]            = ">=",
    [TOK_LSHIFT]         = "<<",
    [TOK_RSHIFT]         = ">>",
    [TOK_BITXOR]         = "|&",
    [TOK_PLUS_ASSIGN]    = "+=",
    [TOK_MINUS_ASSIGN]   = "-=",
    [TOK_STAR_ASSIGN]    = "*=",
    [TOK_SLASH_ASSIGN]   = "/=",
    [TOK_PERCENT_ASSIGN] = "%=",
    [TOK_CARET_ASSIGN]   = "^=",
    [TOK_INCR]           = "++",
    [TOK_DECR]           = "--",

    [TOK_PLUS]           = "+",
    [TOK_MINUS]          = "-",
    [TOK_STAR]           = "*",
    [TOK_SLASH]          = "/",
    [TOK_PERCENT]        = "%",
    [TOK_CARET]          = "^",
    [TOK_AMP]            = "&",
    [TOK_BITOR]          = "|",
    [TOK_LT]             = "<",
    [TOK_GT]             = ">",
    [TOK_ASSIGN]         = "=",
    [TOK_BANG]           = "!",
    [TOK_TILDE]          = "~",
    [TOK_DOT]            = ".",
    [TOK_HASH]           = "#",
    [TOK_LPAREN]         = "(",
    [TOK_RPAREN]         = ")",
    [TOK_LBRACE]         = "{",
    [TOK_RBRACE]         = "}",
    [TOK_LBRACKET]       = "[",
    [TOK_RBRACKET]       = "]",
    [TOK_SEMI]           = ";",
    [TOK_COMMA]          = ",",
    [TOK_COLON]          = ":",
    [TOK_QUESTION]       = "?",

    [TOK_EOF]             = "EOF",
    [TOK_ERROR]           = "ERROR",
};

const char *cando_token_type_name(CandoTokenType t)
{
    if ((unsigned)t >= (unsigned)TOK_COUNT) return "<unknown>";
    const char *name = TOKEN_TYPE_NAMES[t];
    return name ? name : "<unknown>";
}

/* -------------------------------------------------------------------------
 * Keyword table -- maps identifier text to its keyword CandoTokenType.
 * All entries are kept in a simple linear list; the number of keywords is
 * small enough that a hash table would be premature.
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *text;
    u32         len;
    CandoTokenType   type;
} KeywordEntry;

static const KeywordEntry KEYWORDS[] = {
    { "IF",       2,  TOK_IF       },
    { "ELSE",     4,  TOK_ELSE     },
    { "WHILE",    5,  TOK_WHILE    },
    { "FOR",      3,  TOK_FOR      },
    { "FUNCTION", 8,  TOK_FUNCTION },
    { "CLASS",    5,  TOK_CLASS    },
    { "EXTENDS",  7,  TOK_EXTENDS  },
    { "RETURN",   6,  TOK_RETURN   },
    { "THROW",    5,  TOK_THROW    },
    { "TRY",      3,  TOK_TRY      },
    { "CATCH",    5,  TOK_CATCH    },
    { "FINALY",   6,  TOK_FINALY   },
    { "CONST",    5,  TOK_CONST    },
    { "VAR",      3,  TOK_VAR      },
    { "GLOBAL",   6,  TOK_GLOBAL   },
    { "STATIC",   6,  TOK_STATIC   },
    { "PRIVATE",  7,  TOK_PRIVATE  },
    { "ASYNC",    5,  TOK_ASYNC    },
    { "AWAIT",    5,  TOK_AWAIT    },
    { "THREAD",   6,  TOK_THREAD   },
    { "NULL",     4,  TOK_NULL_KW  },
    { "TRUE",     4,  TOK_TRUE_KW  },
    { "FALSE",    5,  TOK_FALSE_KW },
    { "IN",       2,  TOK_IN       },
    { "OF",       2,  TOK_OF       },
    { "OVER",     4,  TOK_OVER     },
    { "CONTINUE", 8,  TOK_CONTINUE },
    { "BREAK",    5,  TOK_BREAK    },
    { "ALSO",     4,  TOK_ALSO     },
    { "SETTLE",   6,  TOK_SETTLE   },
    { "pipe",     4,  TOK_PIPE_KW  },  /* lower-case: implicit loop var */
};

CandoTokenType cando_keyword_type(const char *ident, u32 len)
{
    /* Only pure all-uppercase or pure all-lowercase identifiers can be
     * keywords.  Mixed-case (e.g. "If", "eLsE") are plain identifiers.  */
    bool has_upper = false, has_lower = false;
    for (u32 i = 0; i < len; i++) {
        if (isupper((unsigned char)ident[i])) has_upper = true;
        if (islower((unsigned char)ident[i])) has_lower = true;
    }
    if (has_upper && has_lower) return TOK_IDENT;

    usize n = CANDO_ARRAY_LEN(KEYWORDS);
    for (usize i = 0; i < n; i++) {
        if (KEYWORDS[i].len != len) continue;
        bool match = true;
        for (u32 j = 0; j < len; j++) {
            if (tolower((unsigned char)KEYWORDS[i].text[j]) !=
                tolower((unsigned char)ident[j])) {
                match = false; break;
            }
        }
        if (match) return KEYWORDS[i].type;
    }
    return TOK_IDENT;
}

/* =========================================================================
 * Internal helpers
 * ======================================================================= */

/* Return the current character without advancing. */
static char lex_peek_char(const CandoLexer *lex)
{
    if (lex->pos >= lex->source_len) return '\0';
    return lex->source[lex->pos];
}

/* Return the character at pos+1 without advancing. */
static char lex_peek_char2(const CandoLexer *lex)
{
    if (lex->pos + 1 >= lex->source_len) return '\0';
    return lex->source[lex->pos + 1];
}

/* Advance the cursor by one character, tracking line/column. */
static char lex_advance(CandoLexer *lex)
{
    if (lex->pos >= lex->source_len) return '\0';
    char c = lex->source[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->line_start = lex->pos;
    }
    return c;
}

/* True if the cursor has reached end-of-source. */
static bool lex_at_end(const CandoLexer *lex)
{
    return lex->pos >= lex->source_len;
}

/* Consume the next character only if it matches `expected`. */
static bool lex_match(CandoLexer *lex, char expected)
{
    if (lex_at_end(lex) || lex->source[lex->pos] != expected) return false;
    lex->pos++;   /* no newline tracking needed: callers avoid \n with this */
    return true;
}

/* Consume the next *two* characters only if they match `c1` followed
 * by `c2`.  Used for three-character operators like `~!>` and `~&>`
 * after the leading character has been consumed.                       */
static bool lex_match2(CandoLexer *lex, char c1, char c2)
{
    if (lex->pos + 1 >= lex->source_len)            return false;
    if (lex->source[lex->pos]     != c1)            return false;
    if (lex->source[lex->pos + 1] != c2)            return false;
    lex->pos += 2;
    return true;
}

/* Build a token at the given start position. */
static CandoToken make_token(const CandoLexer *lex,
                              CandoTokenType type,
                              usize start_pos,
                              u32 start_line,
                              usize start_line_start)
{
    CandoToken tok;
    tok.type   = type;
    tok.start  = lex->source + start_pos;
    tok.length = (u32)(lex->pos - start_pos);
    tok.line   = start_line;
    tok.col    = (u32)(start_pos - start_line_start) + 1;
    return tok;
}

/* Build a TOK_ERROR token and populate lex->error_msg. */
static CandoToken lex_error(CandoLexer *lex,
                             usize start_pos,
                             u32 start_line,
                             usize start_line_start,
                             const char *msg)
{
    snprintf(lex->error_msg, sizeof(lex->error_msg), "%s", msg);
    return make_token(lex, TOK_ERROR, start_pos, start_line, start_line_start);
}

/* Skip a // line comment (already consumed the first '/'). */
static void skip_line_comment(CandoLexer *lex)
{
    while (!lex_at_end(lex) && lex_peek_char(lex) != '\n')
        lex_advance(lex);
}

/* Skip a block comment (already consumed the opening slash-star).
 * Scans until the closing star-slash pair is found. */
static bool skip_block_comment(CandoLexer *lex,
                                usize start_pos,
                                u32 start_line,
                                usize start_line_start,
                                CandoToken *err_out)
{
    while (!lex_at_end(lex)) {
        char c = lex_advance(lex);
        if (c == '*' && lex_peek_char(lex) == '/') {
            lex_advance(lex); /* consume '/' */
            return true;
        }
    }
    *err_out = lex_error(lex, start_pos, start_line, start_line_start,
                         "unterminated block comment");
    return false;
}

/* Scan a quoted string that ends with `delim` and produces token type
 * `type`.  Backslash escapes the next character.  Newlines are
 * permitted (the caller's choice of delimiter decides whether the
 * language allows it -- "..." in practice contains no newlines, '...'
 * is multiline, but lex-side both forms accept them and the parser
 * does the per-form interpretation).                                  */
static CandoToken lex_string_quoted(CandoLexer *lex,
                                    usize start_pos,
                                    u32 start_line,
                                    usize start_line_start,
                                    char delim,
                                    CandoTokenType type,
                                    const char *unterminated_msg)
{
    while (!lex_at_end(lex)) {
        char c = lex_advance(lex);
        if (c == '\\') {
            if (lex_at_end(lex)) break;
            lex_advance(lex); /* skip escaped character */
        } else if (c == delim) {
            return make_token(lex, type,
                              start_pos, start_line, start_line_start);
        }
    }
    return lex_error(lex, start_pos, start_line, start_line_start,
                     unterminated_msg);
}

/* Scan a double-quoted string "...". Opening '"' already consumed. */
static CandoToken lex_string_dq(CandoLexer *lex,
                                usize start_pos,
                                u32 start_line,
                                usize start_line_start)
{
    return lex_string_quoted(lex, start_pos, start_line, start_line_start,
                             '"', TOK_STRING_DQ,
                             "unterminated double-quoted string");
}

/* Scan a single-quoted multiline string '...'.  Opening '\'' already
 * consumed.  Newlines are allowed. */
static CandoToken lex_string_sq(CandoLexer *lex,
                                usize start_pos,
                                u32 start_line,
                                usize start_line_start)
{
    return lex_string_quoted(lex, start_pos, start_line, start_line_start,
                             '\'', TOK_STRING_SQ,
                             "unterminated single-quoted string");
}

/* Scan a backtick interpolated string `...`.
 * The opening '`' has already been consumed.
 * ${...} interpolation regions are scanned with brace counting so that
 * nested braces inside ${} do not terminate the template prematurely.
 * The whole token (delimiters and all) is returned as TOK_STRING_BT;
 * the parser splits it into literal / expression fragments as needed. */
static CandoToken lex_string_bt(CandoLexer *lex,
                                 usize start_pos,
                                 u32 start_line,
                                 usize start_line_start)
{
    while (!lex_at_end(lex)) {
        char c = lex_advance(lex);
        if (c == '\\') {
            if (lex_at_end(lex)) break;
            lex_advance(lex);
        } else if (c == '$' && lex_peek_char(lex) == '{') {
            /* Enter interpolation: scan past the matching '}'. */
            lex_advance(lex); /* consume '{' */
            int depth = 1;
            while (!lex_at_end(lex) && depth > 0) {
                char ic = lex_advance(lex);
                if      (ic == '{') depth++;
                else if (ic == '}') depth--;
            }
            if (depth != 0) {
                return lex_error(lex, start_pos, start_line, start_line_start,
                                 "unterminated interpolation in backtick string");
            }
        } else if (c == '`') {
            return make_token(lex, TOK_STRING_BT,
                              start_pos, start_line, start_line_start);
        }
    }
    return lex_error(lex, start_pos, start_line, start_line_start,
                     "unterminated backtick string");
}

/* Scan a numeric literal: DIGIT { DIGIT } [ '.' DIGIT { DIGIT } ]
 * The first digit has already been consumed. */
static CandoToken lex_number(CandoLexer *lex,
                              usize start_pos,
                              u32 start_line,
                              usize start_line_start)
{
    while (isdigit((unsigned char)lex_peek_char(lex)))
        lex_advance(lex);

    /* Optional fractional part. */
    if (lex_peek_char(lex) == '.' && isdigit((unsigned char)lex_peek_char2(lex))) {
        lex_advance(lex); /* consume '.' */
        while (isdigit((unsigned char)lex_peek_char(lex)))
            lex_advance(lex);
    }

    return make_token(lex, TOK_NUMBER, start_pos, start_line, start_line_start);
}

/* Scan an identifier or keyword.  The first character has already been
 * consumed.  Identifiers may contain letters, digits, and underscores. */
static CandoToken lex_ident(CandoLexer *lex,
                             usize start_pos,
                             u32 start_line,
                             usize start_line_start)
{
    while (!lex_at_end(lex)) {
        char c = lex_peek_char(lex);
        if (isalnum((unsigned char)c) || c == '_')
            lex_advance(lex);
        else
            break;
    }

    u32 len = (u32)(lex->pos - start_pos);
    CandoTokenType kw = cando_keyword_type(lex->source + start_pos, len);
    return make_token(lex, kw, start_pos, start_line, start_line_start);
}

/* =========================================================================
 * Public API
 * ======================================================================= */

void cando_lexer_init(CandoLexer *lex, const char *source, usize source_len)
{
    CANDO_ASSERT(lex != NULL);
    CANDO_ASSERT(source != NULL || source_len == 0);
    lex->source     = source;
    lex->source_len = source_len;
    lex->pos        = 0;
    lex->line       = 1;
    lex->line_start = 0;
    lex->error_msg[0] = '\0';
}

CandoToken cando_lexer_next(CandoLexer *lex)
{
    CANDO_ASSERT(lex != NULL);

restart:
    /* Skip whitespace. */
    while (!lex_at_end(lex) && isspace((unsigned char)lex_peek_char(lex)))
        lex_advance(lex);

    if (lex_at_end(lex)) {
        return make_token(lex, TOK_EOF, lex->pos, lex->line, lex->line_start);
    }

    usize start_pos        = lex->pos;
    u32   start_line       = lex->line;
    usize start_line_start = lex->line_start;

    /* Local shorthand: every token in this function shares the same
     * captured-trio start_pos / start_line / start_line_start.            */
#define EMIT(type)  make_token(lex, (type), start_pos, start_line, start_line_start)
#define EMIT_ERR(msg) lex_error(lex, start_pos, start_line, start_line_start, (msg))

    char c = lex_advance(lex);

    /* ---- Single-character tokens whose first char is unambiguous -------- */
    switch (c) {
    case '(': return EMIT(TOK_LPAREN);
    case ')': return EMIT(TOK_RPAREN);
    case '{': return EMIT(TOK_LBRACE);
    case '}': return EMIT(TOK_RBRACE);
    case '[': return EMIT(TOK_LBRACKET);
    case ']': return EMIT(TOK_RBRACKET);
    case ';': return EMIT(TOK_SEMI);
    case ',': return EMIT(TOK_COMMA);
    case '#': return EMIT(TOK_HASH);
    case '^':
        return EMIT(lex_match(lex, '=') ? TOK_CARET_ASSIGN   : TOK_CARET);
    case '%':
        return EMIT(lex_match(lex, '=') ? TOK_PERCENT_ASSIGN : TOK_PERCENT);
    case '*':
        return EMIT(lex_match(lex, '=') ? TOK_STAR_ASSIGN    : TOK_STAR);

    /* ---- '/' : divide, /=, or comment ---------------------------------- */
    case '/':
        if (lex_peek_char(lex) == '/') {
            lex_advance(lex);
            skip_line_comment(lex);
            goto restart;
        }
        if (lex_peek_char(lex) == '*') {
            lex_advance(lex);
            CandoToken err;
            if (!skip_block_comment(lex, start_pos, start_line,
                                    start_line_start, &err))
                return err;
            goto restart;
        }
        return EMIT(lex_match(lex, '=') ? TOK_SLASH_ASSIGN : TOK_SLASH);

    /* ---- '+' : plus, +=, ++ -------------------------------------------- */
    case '+':
        if (lex_match(lex, '+'))   return EMIT(TOK_INCR);
        if (lex_match(lex, '='))   return EMIT(TOK_PLUS_ASSIGN);
        return EMIT(TOK_PLUS);

    /* ---- '-' : minus, -=, --, -> --------------------------------------- */
    case '-':
        if (lex_match(lex, '-'))   return EMIT(TOK_DECR);
        if (lex_match(lex, '='))   return EMIT(TOK_MINUS_ASSIGN);
        if (lex_match(lex, '>'))   return EMIT(TOK_RANGE_ASC);
        return EMIT(TOK_MINUS);

    /* ---- '<' : lt, <=, <<, <- ------------------------------------------ */
    case '<':
        if (lex_match(lex, '='))   return EMIT(TOK_LEQ);
        if (lex_match(lex, '<'))   return EMIT(TOK_LSHIFT);
        if (lex_match(lex, '-'))   return EMIT(TOK_RANGE_DESC);
        return EMIT(TOK_LT);

    /* ---- '>' : gt, >=, >> ---------------------------------------------- */
    case '>':
        if (lex_match(lex, '='))   return EMIT(TOK_GEQ);
        if (lex_match(lex, '>'))   return EMIT(TOK_RSHIFT);
        return EMIT(TOK_GT);

    /* ---- '=' : assign, ==, => ------------------------------------------ */
    case '=':
        if (lex_match(lex, '='))   return EMIT(TOK_EQ);
        if (lex_match(lex, '>'))   return EMIT(TOK_FAT_ARROW);
        return EMIT(TOK_ASSIGN);

    /* ---- '!' : bang, != ------------------------------------------------ */
    case '!':
        return EMIT(lex_match(lex, '=') ? TOK_NEQ : TOK_BANG);

    /* ---- '&' : amp, && ------------------------------------------------- */
    case '&':
        return EMIT(lex_match(lex, '&') ? TOK_AND : TOK_AMP);

    /* ---- '|' : bitor, ||, |& ------------------------------------------ */
    case '|':
        if (lex_match(lex, '|'))   return EMIT(TOK_OR);
        if (lex_match(lex, '&'))   return EMIT(TOK_BITXOR);
        return EMIT(TOK_BITOR);

    /* ---- ':' : colon, :: ----------------------------------------------- */
    case ':':
        return EMIT(lex_match(lex, ':') ? TOK_FLUENT : TOK_COLON);

    /* ---- '.' : dot or ... (vararg) ------------------------------------- */
    case '.':
        if (lex_peek_char(lex) == '.' && lex_peek_char2(lex) == '.') {
            lex_advance(lex); /* second '.' */
            lex_advance(lex); /* third  '.' */
            return EMIT(TOK_VARARG);
        }
        return EMIT(TOK_DOT);

    /* ---- '~' : tilde, ~>, ~!>, ~&> ----------------------------------- */
    case '~':
        if (lex_match2(lex, '!', '>'))  return EMIT(TOK_FILTER_OP);
        if (lex_match2(lex, '&', '>'))  return EMIT(TOK_COND_FILTER_OP);
        if (lex_match(lex, '>'))        return EMIT(TOK_PIPE_OP);
        return EMIT(TOK_TILDE);

    /* ---- '?' : question, ?., ?[ --------------------------------------- */
    case '?':
        if (lex_match(lex, '.'))   return EMIT(TOK_QDOT);
        if (lex_match(lex, '['))   return EMIT(TOK_QLBRACKET);
        return EMIT(TOK_QUESTION);

    /* ---- String literals ----------------------------------------------- */
    case '"':  return lex_string_dq(lex, start_pos, start_line, start_line_start);
    case '\'': return lex_string_sq(lex, start_pos, start_line, start_line_start);
    case '`':  return lex_string_bt(lex, start_pos, start_line, start_line_start);

    default:
        break;
    }

    /* ---- Numeric literal ----------------------------------------------- */
    if (isdigit((unsigned char)c)) {
        return lex_number(lex, start_pos, start_line, start_line_start);
    }

    /* ---- Identifier or keyword ----------------------------------------- */
    if (isalpha((unsigned char)c) || c == '_') {
        return lex_ident(lex, start_pos, start_line, start_line_start);
    }

    /* ---- Unrecognised character ----------------------------------------
     * For printable ASCII we name the character directly; for everything
     * else (control bytes, non-ASCII high bytes) we fall back to the hex
     * escape.  The parser's error_at() prefix already carries the line
     * number, so we deliberately omit it here.                            */
    {
        char msg[96];
        if (c >= 0x20 && c < 0x7F) {
            snprintf(msg, sizeof(msg),
                     "unexpected character '%c'", c);
        } else if ((unsigned char)c >= 0x80) {
            snprintf(msg, sizeof(msg),
                     "unexpected non-ASCII character (byte 0x%02X) -- "
                     "identifiers must be ASCII",
                     (unsigned char)c);
        } else {
            snprintf(msg, sizeof(msg),
                     "unexpected character '\\x%02X'",
                     (unsigned char)c);
        }
        return EMIT_ERR(msg);
    }

#undef EMIT
#undef EMIT_ERR
}

CandoToken cando_lexer_peek(CandoLexer *lex)
{
    /* Save mutable state. */
    usize saved_pos        = lex->pos;
    u32   saved_line       = lex->line;
    usize saved_line_start = lex->line_start;
    char  saved_err[256];
    memcpy(saved_err, lex->error_msg, sizeof(saved_err));

    CandoToken tok = cando_lexer_next(lex);

    /* Restore state. */
    lex->pos        = saved_pos;
    lex->line       = saved_line;
    lex->line_start = saved_line_start;
    memcpy(lex->error_msg, saved_err, sizeof(saved_err));

    return tok;
}

const char *cando_lexer_error_msg(const CandoLexer *lex)
{
    if (lex->error_msg[0] == '\0') return NULL;
    return lex->error_msg;
}
