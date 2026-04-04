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

/* Maps every CdoTokenType to a short printable name. */
const char *cando_token_type_name(CdoTokenType t)
{
    switch (t) {
    case TOK_NUMBER:         return "NUMBER";
    case TOK_STRING_DQ:      return "STRING_DQ";
    case TOK_STRING_SQ:      return "STRING_SQ";
    case TOK_STRING_BT:      return "STRING_BT";
    case TOK_IDENT:          return "IDENT";

    case TOK_IF:             return "IF";
    case TOK_ELSE:           return "ELSE";
    case TOK_WHILE:          return "WHILE";
    case TOK_FOR:            return "FOR";
    case TOK_FUNCTION:       return "FUNCTION";
    case TOK_CLASS:          return "CLASS";
    case TOK_RETURN:         return "RETURN";
    case TOK_THROW:          return "THROW";
    case TOK_TRY:            return "TRY";
    case TOK_CATCH:          return "CATCH";
    case TOK_FINALY:         return "FINALY";
    case TOK_CONST:          return "CONST";
    case TOK_VAR:            return "VAR";
    case TOK_GLOBAL:         return "GLOBAL";
    case TOK_STATIC:         return "STATIC";
    case TOK_PRIVATE:        return "PRIVATE";
    case TOK_ASYNC:          return "ASYNC";
    case TOK_AWAIT:          return "AWAIT";
    case TOK_THREAD:         return "THREAD";
    case TOK_NULL_KW:        return "NULL";
    case TOK_TRUE_KW:        return "TRUE";
    case TOK_FALSE_KW:       return "FALSE";
    case TOK_IN:             return "IN";
    case TOK_OF:             return "OF";
    case TOK_OVER:           return "OVER";
    case TOK_CONTINUE:       return "CONTINUE";
    case TOK_BREAK:          return "BREAK";
    case TOK_PIPE_KW:        return "pipe";

    case TOK_PIPE_OP:        return "~>";
    case TOK_FILTER_OP:      return "~!>";
    case TOK_RANGE_ASC:      return "->";
    case TOK_RANGE_DESC:     return "<-";
    case TOK_FLUENT:         return "::";
    case TOK_VARARG:         return "...";
    case TOK_FAT_ARROW:      return "=>";
    case TOK_AND:            return "&&";
    case TOK_OR:             return "||";
    case TOK_EQ:             return "==";
    case TOK_NEQ:            return "!=";
    case TOK_LEQ:            return "<=";
    case TOK_GEQ:            return ">=";
    case TOK_LSHIFT:         return "<<";
    case TOK_RSHIFT:         return ">>";
    case TOK_BITXOR:         return "|&";
    case TOK_PLUS_ASSIGN:    return "+=";
    case TOK_MINUS_ASSIGN:   return "-=";
    case TOK_STAR_ASSIGN:    return "*=";
    case TOK_SLASH_ASSIGN:   return "/=";
    case TOK_PERCENT_ASSIGN: return "%=";
    case TOK_CARET_ASSIGN:   return "^=";
    case TOK_INCR:           return "++";
    case TOK_DECR:           return "--";

    case TOK_PLUS:           return "+";
    case TOK_MINUS:          return "-";
    case TOK_STAR:           return "*";
    case TOK_SLASH:          return "/";
    case TOK_PERCENT:        return "%";
    case TOK_CARET:          return "^";
    case TOK_AMP:            return "&";
    case TOK_BITOR:          return "|";
    case TOK_LT:             return "<";
    case TOK_GT:             return ">";
    case TOK_ASSIGN:         return "=";
    case TOK_BANG:           return "!";
    case TOK_TILDE:          return "~";
    case TOK_DOT:            return ".";
    case TOK_HASH:           return "#";
    case TOK_LPAREN:         return "(";
    case TOK_RPAREN:         return ")";
    case TOK_LBRACE:         return "{";
    case TOK_RBRACE:         return "}";
    case TOK_LBRACKET:       return "[";
    case TOK_RBRACKET:       return "]";
    case TOK_SEMI:           return ";";
    case TOK_COMMA:          return ",";
    case TOK_COLON:          return ":";

    case TOK_EOF:            return "EOF";
    case TOK_ERROR:          return "ERROR";
    case TOK_COUNT:          return "<COUNT>";
    }
    return "<unknown>";
}

/* -------------------------------------------------------------------------
 * Keyword table -- maps identifier text to its keyword CdoTokenType.
 * All entries are kept in a simple linear list; the number of keywords is
 * small enough that a hash table would be premature.
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *text;
    u32         len;
    CdoTokenType   type;
} KeywordEntry;

static const KeywordEntry KEYWORDS[] = {
    { "IF",       2,  TOK_IF       },
    { "ELSE",     4,  TOK_ELSE     },
    { "WHILE",    5,  TOK_WHILE    },
    { "FOR",      3,  TOK_FOR      },
    { "FUNCTION", 8,  TOK_FUNCTION },
    { "CLASS",    5,  TOK_CLASS    },
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
    { "pipe",     4,  TOK_PIPE_KW  },  /* lower-case: implicit loop var */
};

CdoTokenType cando_keyword_type(const char *ident, u32 len)
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

/* Build a token at the given start position. */
static CandoToken make_token(const CandoLexer *lex,
                              CdoTokenType type,
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

/* Scan a double-quoted string "...". Handles \\, \", and other escapes.
 * The opening '"' has already been consumed.  Scans up to the closing '"'.
 * The token includes both delimiters. */
static CandoToken lex_string_dq(CandoLexer *lex,
                                 usize start_pos,
                                 u32 start_line,
                                 usize start_line_start)
{
    while (!lex_at_end(lex)) {
        char c = lex_advance(lex);
        if (c == '\\') {
            if (lex_at_end(lex)) break;
            lex_advance(lex); /* skip escaped character */
        } else if (c == '"') {
            return make_token(lex, TOK_STRING_DQ,
                              start_pos, start_line, start_line_start);
        }
    }
    return lex_error(lex, start_pos, start_line, start_line_start,
                     "unterminated double-quoted string");
}

/* Scan a single-quoted multiline string '...'.
 * The opening '\'' has already been consumed.  Newlines are allowed. */
static CandoToken lex_string_sq(CandoLexer *lex,
                                 usize start_pos,
                                 u32 start_line,
                                 usize start_line_start)
{
    while (!lex_at_end(lex)) {
        char c = lex_advance(lex);
        if (c == '\\') {
            if (lex_at_end(lex)) break;
            lex_advance(lex);
        } else if (c == '\'') {
            return make_token(lex, TOK_STRING_SQ,
                              start_pos, start_line, start_line_start);
        }
    }
    return lex_error(lex, start_pos, start_line, start_line_start,
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
    CdoTokenType kw = cando_keyword_type(lex->source + start_pos, len);
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

    char c = lex_advance(lex);

    /* ---- Single-character tokens whose first char is unambiguous -------- */
    switch (c) {
    case '(': return make_token(lex, TOK_LPAREN,   start_pos, start_line, start_line_start);
    case ')': return make_token(lex, TOK_RPAREN,   start_pos, start_line, start_line_start);
    case '{': return make_token(lex, TOK_LBRACE,   start_pos, start_line, start_line_start);
    case '}': return make_token(lex, TOK_RBRACE,   start_pos, start_line, start_line_start);
    case '[': return make_token(lex, TOK_LBRACKET, start_pos, start_line, start_line_start);
    case ']': return make_token(lex, TOK_RBRACKET, start_pos, start_line, start_line_start);
    case ';': return make_token(lex, TOK_SEMI,     start_pos, start_line, start_line_start);
    case ',': return make_token(lex, TOK_COMMA,    start_pos, start_line, start_line_start);
    case '#': return make_token(lex, TOK_HASH,     start_pos, start_line, start_line_start);
    case '^':
        if (lex_match(lex, '='))
            return make_token(lex, TOK_CARET_ASSIGN, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_CARET, start_pos, start_line, start_line_start);
    case '%':
        if (lex_match(lex, '='))
            return make_token(lex, TOK_PERCENT_ASSIGN, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_PERCENT, start_pos, start_line, start_line_start);
    case '*':
        if (lex_match(lex, '='))
            return make_token(lex, TOK_STAR_ASSIGN, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_STAR, start_pos, start_line, start_line_start);

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
        if (lex_match(lex, '='))
            return make_token(lex, TOK_SLASH_ASSIGN, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_SLASH, start_pos, start_line, start_line_start);

    /* ---- '+' : plus, +=, ++ -------------------------------------------- */
    case '+':
        if (lex_match(lex, '+'))
            return make_token(lex, TOK_INCR, start_pos, start_line, start_line_start);
        if (lex_match(lex, '='))
            return make_token(lex, TOK_PLUS_ASSIGN, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_PLUS, start_pos, start_line, start_line_start);

    /* ---- '-' : minus, -=, --, -> --------------------------------------- */
    case '-':
        if (lex_match(lex, '-'))
            return make_token(lex, TOK_DECR, start_pos, start_line, start_line_start);
        if (lex_match(lex, '='))
            return make_token(lex, TOK_MINUS_ASSIGN, start_pos, start_line, start_line_start);
        if (lex_match(lex, '>'))
            return make_token(lex, TOK_RANGE_ASC, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_MINUS, start_pos, start_line, start_line_start);

    /* ---- '<' : lt, <=, <<, <- ------------------------------------------ */
    case '<':
        if (lex_match(lex, '='))
            return make_token(lex, TOK_LEQ, start_pos, start_line, start_line_start);
        if (lex_match(lex, '<'))
            return make_token(lex, TOK_LSHIFT, start_pos, start_line, start_line_start);
        if (lex_match(lex, '-'))
            return make_token(lex, TOK_RANGE_DESC, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_LT, start_pos, start_line, start_line_start);

    /* ---- '>' : gt, >=, >> ---------------------------------------------- */
    case '>':
        if (lex_match(lex, '='))
            return make_token(lex, TOK_GEQ, start_pos, start_line, start_line_start);
        if (lex_match(lex, '>'))
            return make_token(lex, TOK_RSHIFT, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_GT, start_pos, start_line, start_line_start);

    /* ---- '=' : assign, ==, => ------------------------------------------ */
    case '=':
        if (lex_match(lex, '='))
            return make_token(lex, TOK_EQ, start_pos, start_line, start_line_start);
        if (lex_match(lex, '>'))
            return make_token(lex, TOK_FAT_ARROW, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_ASSIGN, start_pos, start_line, start_line_start);

    /* ---- '!' : bang, != ------------------------------------------------ */
    case '!':
        if (lex_match(lex, '='))
            return make_token(lex, TOK_NEQ, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_BANG, start_pos, start_line, start_line_start);

    /* ---- '&' : amp, && ------------------------------------------------- */
    case '&':
        if (lex_match(lex, '&'))
            return make_token(lex, TOK_AND, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_AMP, start_pos, start_line, start_line_start);

    /* ---- '|' : bitor, ||, |& ------------------------------------------ */
    case '|':
        if (lex_match(lex, '|'))
            return make_token(lex, TOK_OR, start_pos, start_line, start_line_start);
        if (lex_match(lex, '&'))
            return make_token(lex, TOK_BITXOR, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_BITOR, start_pos, start_line, start_line_start);

    /* ---- ':' : colon, :: ----------------------------------------------- */
    case ':':
        if (lex_match(lex, ':'))
            return make_token(lex, TOK_FLUENT, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_COLON, start_pos, start_line, start_line_start);

    /* ---- '.' : dot or ... (vararg) ------------------------------------- */
    case '.':
        if (lex_peek_char(lex) == '.' && lex_peek_char2(lex) == '.') {
            lex_advance(lex); /* second '.' */
            lex_advance(lex); /* third  '.' */
            return make_token(lex, TOK_VARARG, start_pos, start_line, start_line_start);
        }
        return make_token(lex, TOK_DOT, start_pos, start_line, start_line_start);

    /* ---- '~' : tilde, ~>, ~!> ----------------------------------------- */
    case '~':
        if (lex_peek_char(lex) == '!') {
            /* Could be ~!> */
            if (lex->pos + 1 < lex->source_len &&
                lex->source[lex->pos + 1] == '>') {
                lex_advance(lex); /* '!' */
                lex_advance(lex); /* '>' */
                return make_token(lex, TOK_FILTER_OP, start_pos, start_line, start_line_start);
            }
        }
        if (lex_match(lex, '>'))
            return make_token(lex, TOK_PIPE_OP, start_pos, start_line, start_line_start);
        return make_token(lex, TOK_TILDE, start_pos, start_line, start_line_start);

    /* ---- String literals ----------------------------------------------- */
    case '"':
        return lex_string_dq(lex, start_pos, start_line, start_line_start);
    case '\'':
        return lex_string_sq(lex, start_pos, start_line, start_line_start);
    case '`':
        return lex_string_bt(lex, start_pos, start_line, start_line_start);

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

    /* ---- Unrecognised character ---------------------------------------- */
    {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "unexpected character '\\x%02X' (line %u)",
                 (unsigned char)c, start_line);
        return lex_error(lex, start_pos, start_line, start_line_start, msg);
    }
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
