/*
 * tests/test_lexer.c -- Unit tests for source/parser/token.h and lexer.h/c
 *
 * Compile:
 *   gcc -std=c11 -I source/core -I source/parser \
 *       source/core/common.c source/parser/lexer.c \
 *       tests/test_lexer.c -o tests/test_lexer && ./tests/test_lexer
 *
 * Exit 0 on success, non-zero on failure.
 */

#include "common.h"
#include "token.h"
#include "lexer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Minimal test harness (mirrors test_core.c style)
 * ------------------------------------------------------------------------ */
static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) static void name(void)

#define EXPECT(cond) \
    do { \
        g_tests_run++; \
        if (cond) { \
            g_tests_passed++; \
        } else { \
            g_tests_failed++; \
            fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define EXPECT_EQ(a, b)  EXPECT((a) == (b))
#define EXPECT_NEQ(a, b) EXPECT((a) != (b))
#define EXPECT_TRUE(x)   EXPECT(!!(x))
#define EXPECT_FALSE(x)  EXPECT(!(x))
#define EXPECT_STR(a, b) EXPECT(strcmp((a), (b)) == 0)

static void run_test(const char *name, void (*fn)(void))
{
    printf("  %-50s ", name);
    fflush(stdout);
    int before = g_tests_failed;
    fn();
    printf("%s\n", (g_tests_failed == before) ? "OK" : "FAILED");
}

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

/* Lex `src` and return the type of the Nth token (0-based, skipping EOF). */
static CandoToken lex_nth(const char *src, int n)
{
    CandoLexer lex;
    cando_lexer_init(&lex, src, strlen(src));
    CandoToken tok;
    for (int i = 0; i <= n; i++) {
        tok = cando_lexer_next(&lex);
        if (tok.type == TOK_EOF || tok.type == TOK_ERROR) break;
    }
    return tok;
}

/* Lex `src` and collect all non-EOF tokens into an array.
 * Caller must free the returned array. */
static CandoToken *lex_all(const char *src, u32 *out_count)
{
    CandoLexer lex;
    cando_lexer_init(&lex, src, strlen(src));

    u32 cap = 16, count = 0;
    CandoToken *arr = malloc(cap * sizeof(CandoToken));

    for (;;) {
        CandoToken tok = cando_lexer_next(&lex);
        if (tok.type == TOK_EOF || tok.type == TOK_ERROR) break;
        if (count == cap) {
            cap *= 2;
            arr = realloc(arr, cap * sizeof(CandoToken));
        }
        arr[count++] = tok;
    }
    *out_count = count;
    return arr;
}

/* -------------------------------------------------------------------------
 * Tests: token type names
 * ------------------------------------------------------------------------ */
TEST(test_token_type_names)
{
    EXPECT_STR(cando_token_type_name(TOK_NUMBER),  "NUMBER");
    EXPECT_STR(cando_token_type_name(TOK_IDENT),   "IDENT");
    EXPECT_STR(cando_token_type_name(TOK_IF),      "IF");
    EXPECT_STR(cando_token_type_name(TOK_WHILE),   "WHILE");
    EXPECT_STR(cando_token_type_name(TOK_PIPE_OP), "~>");
    EXPECT_STR(cando_token_type_name(TOK_FILTER_OP), "~!>");
    EXPECT_STR(cando_token_type_name(TOK_EOF),     "EOF");
    EXPECT_STR(cando_token_type_name(TOK_ERROR),   "ERROR");
}

/* -------------------------------------------------------------------------
 * Tests: keyword lookup
 * ------------------------------------------------------------------------ */
TEST(test_keyword_lookup)
{
    EXPECT_EQ(cando_keyword_type("IF",       2), TOK_IF);
    EXPECT_EQ(cando_keyword_type("ELSE",     4), TOK_ELSE);
    EXPECT_EQ(cando_keyword_type("WHILE",    5), TOK_WHILE);
    EXPECT_EQ(cando_keyword_type("FOR",      3), TOK_FOR);
    EXPECT_EQ(cando_keyword_type("FUNCTION", 8), TOK_FUNCTION);
    EXPECT_EQ(cando_keyword_type("CLASS",    5), TOK_CLASS);
    EXPECT_EQ(cando_keyword_type("RETURN",   6), TOK_RETURN);
    EXPECT_EQ(cando_keyword_type("THROW",    5), TOK_THROW);
    EXPECT_EQ(cando_keyword_type("TRY",      3), TOK_TRY);
    EXPECT_EQ(cando_keyword_type("CATCH",    5), TOK_CATCH);
    EXPECT_EQ(cando_keyword_type("FINALY",   6), TOK_FINALY);
    EXPECT_EQ(cando_keyword_type("CONST",    5), TOK_CONST);
    EXPECT_EQ(cando_keyword_type("VAR",      3), TOK_VAR);
    EXPECT_EQ(cando_keyword_type("GLOBAL",   6), TOK_GLOBAL);
    EXPECT_EQ(cando_keyword_type("STATIC",   6), TOK_STATIC);
    EXPECT_EQ(cando_keyword_type("PRIVATE",  7), TOK_PRIVATE);
    EXPECT_EQ(cando_keyword_type("ASYNC",    5), TOK_ASYNC);
    EXPECT_EQ(cando_keyword_type("AWAIT",    5), TOK_AWAIT);
    EXPECT_EQ(cando_keyword_type("NULL",     4), TOK_NULL_KW);
    EXPECT_EQ(cando_keyword_type("TRUE",     4), TOK_TRUE_KW);
    EXPECT_EQ(cando_keyword_type("FALSE",    5), TOK_FALSE_KW);
    EXPECT_EQ(cando_keyword_type("IN",       2), TOK_IN);
    EXPECT_EQ(cando_keyword_type("OF",       2), TOK_OF);
    EXPECT_EQ(cando_keyword_type("OVER",     4), TOK_OVER);
    EXPECT_EQ(cando_keyword_type("CONTINUE", 8), TOK_CONTINUE);
    EXPECT_EQ(cando_keyword_type("BREAK",    5), TOK_BREAK);
    EXPECT_EQ(cando_keyword_type("pipe",     4), TOK_PIPE_KW);

    /* Lower-case keywords are also recognised */
    EXPECT_EQ(cando_keyword_type("if",       2), TOK_IF);
    EXPECT_EQ(cando_keyword_type("else",     4), TOK_ELSE);
    EXPECT_EQ(cando_keyword_type("while",    5), TOK_WHILE);
    EXPECT_EQ(cando_keyword_type("for",      3), TOK_FOR);
    EXPECT_EQ(cando_keyword_type("function", 8), TOK_FUNCTION);
    EXPECT_EQ(cando_keyword_type("return",   6), TOK_RETURN);
    EXPECT_EQ(cando_keyword_type("var",      3), TOK_VAR);
    EXPECT_EQ(cando_keyword_type("PIPE",     4), TOK_PIPE_KW); /* upper-case pipe */

    /* Non-keywords: plain identifiers and mixed-case are TOK_IDENT */
    EXPECT_EQ(cando_keyword_type("foo",    3), TOK_IDENT);
    EXPECT_EQ(cando_keyword_type("IFALL",  5), TOK_IDENT); /* prefix only */
    EXPECT_EQ(cando_keyword_type("iF",     2), TOK_IDENT); /* mixed-case */
    EXPECT_EQ(cando_keyword_type("If",     2), TOK_IDENT); /* mixed-case */
    EXPECT_EQ(cando_keyword_type("Else",   4), TOK_IDENT); /* mixed-case */
    EXPECT_EQ(cando_keyword_type("Return", 6), TOK_IDENT); /* mixed-case */
}

/* -------------------------------------------------------------------------
 * Tests: lexer init and EOF
 * ------------------------------------------------------------------------ */
TEST(test_empty_source)
{
    CandoLexer lex;
    cando_lexer_init(&lex, "", 0);
    CandoToken tok = cando_lexer_next(&lex);
    EXPECT_EQ(tok.type, TOK_EOF);
    EXPECT_EQ(tok.line, 1u);
}

TEST(test_whitespace_only)
{
    CandoLexer lex;
    const char *src = "   \t\n  \r\n  ";
    cando_lexer_init(&lex, src, strlen(src));
    CandoToken tok = cando_lexer_next(&lex);
    EXPECT_EQ(tok.type, TOK_EOF);
}

/* -------------------------------------------------------------------------
 * Tests: single-character tokens
 * ------------------------------------------------------------------------ */
TEST(test_single_char_tokens)
{
    struct { const char *src; TokenType type; } cases[] = {
        { "(", TOK_LPAREN   }, { ")", TOK_RPAREN   },
        { "{", TOK_LBRACE   }, { "}", TOK_RBRACE   },
        { "[", TOK_LBRACKET }, { "]", TOK_RBRACKET },
        { ";", TOK_SEMI     }, { ",", TOK_COMMA    },
        { "#", TOK_HASH     }, { "^", TOK_CARET    },
        { "%", TOK_PERCENT  }, { "*", TOK_STAR     },
        { "+", TOK_PLUS     }, { "-", TOK_MINUS    },
        { "<", TOK_LT       }, { ">", TOK_GT       },
        { "=", TOK_ASSIGN   }, { "!", TOK_BANG     },
        { "~", TOK_TILDE    }, { ".", TOK_DOT      },
        { "&", TOK_AMP      }, { "|", TOK_BITOR    },
        { ":", TOK_COLON    }, { "/", TOK_SLASH    },
    };
    usize n = CANDO_ARRAY_LEN(cases);
    for (usize i = 0; i < n; i++) {
        CandoToken tok = lex_nth(cases[i].src, 0);
        EXPECT_EQ(tok.type, cases[i].type);
    }
}

/* -------------------------------------------------------------------------
 * Tests: multi-character operators
 * ------------------------------------------------------------------------ */
TEST(test_multi_char_operators)
{
    struct { const char *src; TokenType type; } cases[] = {
        { "~>",  TOK_PIPE_OP     },
        { "~!>", TOK_FILTER_OP   },
        { "->",  TOK_RANGE_ASC   },
        { "<-",  TOK_RANGE_DESC  },
        { "::",  TOK_FLUENT      },
        { "...", TOK_VARARG      },
        { "=>",  TOK_FAT_ARROW   },
        { "&&",  TOK_AND         },
        { "||",  TOK_OR          },
        { "==",  TOK_EQ          },
        { "!=",  TOK_NEQ         },
        { "<=",  TOK_LEQ         },
        { ">=",  TOK_GEQ         },
        { "<<",  TOK_LSHIFT      },
        { ">>",  TOK_RSHIFT      },
        { "|&",  TOK_BITXOR      },
        { "+=",  TOK_PLUS_ASSIGN  },
        { "-=",  TOK_MINUS_ASSIGN },
        { "*=",  TOK_STAR_ASSIGN  },
        { "/=",  TOK_SLASH_ASSIGN },
        { "%=",  TOK_PERCENT_ASSIGN },
        { "^=",  TOK_CARET_ASSIGN  },
        { "++",  TOK_INCR         },
        { "--",  TOK_DECR         },
    };
    usize n = CANDO_ARRAY_LEN(cases);
    for (usize i = 0; i < n; i++) {
        CandoToken tok = lex_nth(cases[i].src, 0);
        if (tok.type != cases[i].type) {
            fprintf(stderr, "  [operator '%s'] expected %s got %s\n",
                    cases[i].src,
                    cando_token_type_name(cases[i].type),
                    cando_token_type_name(tok.type));
        }
        EXPECT_EQ(tok.type, cases[i].type);
    }
}

/* -------------------------------------------------------------------------
 * Tests: number literals
 * ------------------------------------------------------------------------ */
TEST(test_integer_literal)
{
    CandoToken tok = lex_nth("42", 0);
    EXPECT_EQ(tok.type, TOK_NUMBER);
    EXPECT_EQ(tok.length, 2u);
    EXPECT_EQ(tok.line, 1u);
    EXPECT_EQ(tok.col, 1u);
}

TEST(test_float_literal)
{
    CandoToken tok = lex_nth("3.14", 0);
    EXPECT_EQ(tok.type, TOK_NUMBER);
    EXPECT_EQ(tok.length, 4u);
}

TEST(test_number_no_leading_dot)
{
    /* ".5" is NOT a number; "." followed by "5" (identifier) */
    CandoToken tok = lex_nth(".5", 0);
    EXPECT_EQ(tok.type, TOK_DOT);
}

TEST(test_multiple_numbers)
{
    u32 count;
    CandoToken *toks = lex_all("1 22 333", &count);
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(toks[0].type, TOK_NUMBER);
    EXPECT_EQ(toks[0].length, 1u);
    EXPECT_EQ(toks[1].type, TOK_NUMBER);
    EXPECT_EQ(toks[1].length, 2u);
    EXPECT_EQ(toks[2].type, TOK_NUMBER);
    EXPECT_EQ(toks[2].length, 3u);
    free(toks);
}

/* -------------------------------------------------------------------------
 * Tests: identifier and keyword scanning
 * ------------------------------------------------------------------------ */
TEST(test_identifier)
{
    CandoToken tok = lex_nth("myVar", 0);
    EXPECT_EQ(tok.type, TOK_IDENT);
    EXPECT_EQ(tok.length, 5u);
}

TEST(test_underscore_ident)
{
    CandoToken tok = lex_nth("_internal_123", 0);
    EXPECT_EQ(tok.type, TOK_IDENT);
    EXPECT_EQ(tok.length, 13u);
}

TEST(test_keyword_if)
{
    CandoToken tok = lex_nth("IF", 0);
    EXPECT_EQ(tok.type, TOK_IF);
}

TEST(test_keyword_pipe_lowercase)
{
    CandoToken tok = lex_nth("pipe", 0);
    EXPECT_EQ(tok.type, TOK_PIPE_KW);
}

TEST(test_keywords_in_sequence)
{
    u32 count;
    CandoToken *toks = lex_all("VAR CONST GLOBAL", &count);
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(toks[0].type, TOK_VAR);
    EXPECT_EQ(toks[1].type, TOK_CONST);
    EXPECT_EQ(toks[2].type, TOK_GLOBAL);
    free(toks);
}

TEST(test_all_keywords)
{
    /* Verify every keyword is recognised when surrounded by spaces. */
    struct { const char *kw; TokenType expected; } kws[] = {
        {"IF",TOK_IF},{"ELSE",TOK_ELSE},{"WHILE",TOK_WHILE},{"FOR",TOK_FOR},
        {"FUNCTION",TOK_FUNCTION},{"CLASS",TOK_CLASS},{"RETURN",TOK_RETURN},
        {"THROW",TOK_THROW},{"TRY",TOK_TRY},{"CATCH",TOK_CATCH},
        {"FINALY",TOK_FINALY},{"CONST",TOK_CONST},{"VAR",TOK_VAR},
        {"GLOBAL",TOK_GLOBAL},{"STATIC",TOK_STATIC},{"PRIVATE",TOK_PRIVATE},
        {"ASYNC",TOK_ASYNC},{"AWAIT",TOK_AWAIT},{"NULL",TOK_NULL_KW},
        {"TRUE",TOK_TRUE_KW},{"FALSE",TOK_FALSE_KW},{"IN",TOK_IN},
        {"OF",TOK_OF},{"OVER",TOK_OVER},{"CONTINUE",TOK_CONTINUE},
        {"BREAK",TOK_BREAK},{"pipe",TOK_PIPE_KW},
    };
    for (usize i = 0; i < CANDO_ARRAY_LEN(kws); i++) {
        CandoToken tok = lex_nth(kws[i].kw, 0);
        EXPECT_EQ(tok.type, kws[i].expected);
    }
}

/* -------------------------------------------------------------------------
 * Tests: string literals
 * ------------------------------------------------------------------------ */
TEST(test_string_double_quote)
{
    CandoToken tok = lex_nth("\"hello world\"", 0);
    EXPECT_EQ(tok.type, TOK_STRING_DQ);
    EXPECT_EQ(tok.length, 13u);  /* includes the delimiters */
}

TEST(test_string_double_quote_escape)
{
    /* "say \"hi\"" — escaped quotes inside */
    CandoToken tok = lex_nth("\"say \\\"hi\\\"\"", 0);
    EXPECT_EQ(tok.type, TOK_STRING_DQ);
}

TEST(test_string_single_quote)
{
    CandoToken tok = lex_nth("'multi\nline'", 0);
    EXPECT_EQ(tok.type, TOK_STRING_SQ);
    EXPECT_EQ(tok.length, 12u);
}

TEST(test_string_backtick)
{
    CandoToken tok = lex_nth("`hello ${name}!`", 0);
    EXPECT_EQ(tok.type, TOK_STRING_BT);
}

TEST(test_string_backtick_nested_braces)
{
    /* Nested braces inside ${ } must not confuse the scanner. */
    CandoToken tok = lex_nth("`value: ${obj.key}`", 0);
    EXPECT_EQ(tok.type, TOK_STRING_BT);
}

TEST(test_string_unterminated_dq)
{
    CandoLexer lex;
    const char *src = "\"unterminated";
    cando_lexer_init(&lex, src, strlen(src));
    CandoToken tok = cando_lexer_next(&lex);
    EXPECT_EQ(tok.type, TOK_ERROR);
    EXPECT_TRUE(cando_lexer_error_msg(&lex) != NULL);
}

TEST(test_string_unterminated_sq)
{
    CandoLexer lex;
    const char *src = "'unterminated";
    cando_lexer_init(&lex, src, strlen(src));
    CandoToken tok = cando_lexer_next(&lex);
    EXPECT_EQ(tok.type, TOK_ERROR);
}

TEST(test_string_unterminated_bt)
{
    CandoLexer lex;
    const char *src = "`unterminated";
    cando_lexer_init(&lex, src, strlen(src));
    CandoToken tok = cando_lexer_next(&lex);
    EXPECT_EQ(tok.type, TOK_ERROR);
}

/* -------------------------------------------------------------------------
 * Tests: comments
 * ------------------------------------------------------------------------ */
TEST(test_line_comment_skipped)
{
    u32 count;
    CandoToken *toks = lex_all("// this is a comment\n42", &count);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(toks[0].type, TOK_NUMBER);
    free(toks);
}

TEST(test_block_comment_skipped)
{
    u32 count;
    CandoToken *toks = lex_all("/* block\ncomment */ 99", &count);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(toks[0].type, TOK_NUMBER);
    free(toks);
}

TEST(test_unterminated_block_comment)
{
    CandoLexer lex;
    const char *src = "/* no end";
    cando_lexer_init(&lex, src, strlen(src));
    CandoToken tok = cando_lexer_next(&lex);
    EXPECT_EQ(tok.type, TOK_ERROR);
}

/* -------------------------------------------------------------------------
 * Tests: line and column tracking
 * ------------------------------------------------------------------------ */
TEST(test_line_tracking)
{
    CandoLexer lex;
    const char *src = "a\nb\nc";
    cando_lexer_init(&lex, src, strlen(src));

    CandoToken t1 = cando_lexer_next(&lex);
    CandoToken t2 = cando_lexer_next(&lex);
    CandoToken t3 = cando_lexer_next(&lex);

    EXPECT_EQ(t1.line, 1u);
    EXPECT_EQ(t2.line, 2u);
    EXPECT_EQ(t3.line, 3u);
}

TEST(test_column_tracking)
{
    CandoLexer lex;
    const char *src = "abc def";
    cando_lexer_init(&lex, src, strlen(src));

    CandoToken t1 = cando_lexer_next(&lex);
    CandoToken t2 = cando_lexer_next(&lex);

    EXPECT_EQ(t1.col, 1u);
    EXPECT_EQ(t2.col, 5u);
}

/* -------------------------------------------------------------------------
 * Tests: peek
 * ------------------------------------------------------------------------ */
TEST(test_peek_does_not_advance)
{
    CandoLexer lex;
    const char *src = "1 2 3";
    cando_lexer_init(&lex, src, strlen(src));

    CandoToken p1 = cando_lexer_peek(&lex);
    CandoToken p2 = cando_lexer_peek(&lex);  /* same position */
    EXPECT_EQ(p1.type, p2.type);
    EXPECT_EQ(p1.start, p2.start);

    CandoToken n1 = cando_lexer_next(&lex);
    EXPECT_EQ(n1.type, TOK_NUMBER);
    EXPECT_EQ(n1.start[0], '1');

    CandoToken n2 = cando_lexer_next(&lex);
    EXPECT_EQ(n2.type, TOK_NUMBER);
    EXPECT_EQ(n2.start[0], '2');
}

/* -------------------------------------------------------------------------
 * Tests: realistic Cando snippets
 * ------------------------------------------------------------------------ */
TEST(test_var_declaration)
{
    /* VAR x = 42; */
    u32 count;
    CandoToken *toks = lex_all("VAR x = 42;", &count);
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(toks[0].type, TOK_VAR);
    EXPECT_EQ(toks[1].type, TOK_IDENT);
    EXPECT_EQ(toks[2].type, TOK_ASSIGN);
    EXPECT_EQ(toks[3].type, TOK_NUMBER);
    EXPECT_EQ(toks[4].type, TOK_SEMI);
    free(toks);
}

TEST(test_if_statement)
{
    /* IF x == 1, 2 { } */
    u32 count;
    CandoToken *toks = lex_all("IF x == 1, 2 { }", &count);
    EXPECT_EQ(count, 8u);
    EXPECT_EQ(toks[0].type, TOK_IF);
    EXPECT_EQ(toks[1].type, TOK_IDENT);
    EXPECT_EQ(toks[2].type, TOK_EQ);
    EXPECT_EQ(toks[3].type, TOK_NUMBER);
    EXPECT_EQ(toks[4].type, TOK_COMMA);
    EXPECT_EQ(toks[5].type, TOK_NUMBER);
    EXPECT_EQ(toks[6].type, TOK_LBRACE);
    EXPECT_EQ(toks[7].type, TOK_RBRACE);
    free(toks);
}

TEST(test_pipe_operator)
{
    /* raw_data ~> pipe * 10 */
    u32 count;
    CandoToken *toks = lex_all("raw_data ~> pipe * 10", &count);
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(toks[0].type, TOK_IDENT);
    EXPECT_EQ(toks[1].type, TOK_PIPE_OP);
    EXPECT_EQ(toks[2].type, TOK_PIPE_KW);
    EXPECT_EQ(toks[3].type, TOK_STAR);
    EXPECT_EQ(toks[4].type, TOK_NUMBER);
    free(toks);
}

TEST(test_filter_operator)
{
    /* items ~!> pipe > 0 */
    u32 count;
    CandoToken *toks = lex_all("items ~!> pipe > 0", &count);
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(toks[0].type, TOK_IDENT);
    EXPECT_EQ(toks[1].type, TOK_FILTER_OP);
    EXPECT_EQ(toks[2].type, TOK_PIPE_KW);
    EXPECT_EQ(toks[3].type, TOK_GT);
    EXPECT_EQ(toks[4].type, TOK_NUMBER);
    free(toks);
}

TEST(test_range_operators)
{
    /* 1 -> 5 ; 5 <- 1
     * [0]=NUMBER [1]=RANGE_ASC [2]=NUMBER [3]=SEMI
     * [4]=NUMBER [5]=RANGE_DESC [6]=NUMBER */
    u32 count;
    CandoToken *toks = lex_all("1 -> 5 ; 5 <- 1", &count);
    EXPECT_EQ(count, 7u);
    EXPECT_EQ(toks[1].type, TOK_RANGE_ASC);
    EXPECT_EQ(toks[3].type, TOK_SEMI);
    EXPECT_EQ(toks[4].type, TOK_NUMBER);
    EXPECT_EQ(toks[5].type, TOK_RANGE_DESC);
    EXPECT_EQ(toks[6].type, TOK_NUMBER);
    free(toks);
}

TEST(test_fluent_call)
{
    /* obj::method()
     * [0]=IDENT [1]=FLUENT [2]=IDENT [3]=LPAREN [4]=RPAREN */
    u32 count;
    CandoToken *toks = lex_all("obj::method()", &count);
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(toks[0].type, TOK_IDENT);
    EXPECT_EQ(toks[1].type, TOK_FLUENT);
    EXPECT_EQ(toks[2].type, TOK_IDENT);
    EXPECT_EQ(toks[3].type, TOK_LPAREN);
    EXPECT_EQ(toks[4].type, TOK_RPAREN);
    free(toks);
}

TEST(test_vararg)
{
    /* ... */
    u32 count;
    CandoToken *toks = lex_all("...", &count);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(toks[0].type, TOK_VARARG);
    free(toks);
}

TEST(test_mask_operators)
{
    /* Mask pattern inside parens: (~.~) expr
     * Tokens: ( ~ . ~ ) expr */
    u32 count;
    CandoToken *toks = lex_all("(~.~) expr", &count);
    EXPECT_EQ(count, 6u);
    EXPECT_EQ(toks[0].type, TOK_LPAREN);
    EXPECT_EQ(toks[1].type, TOK_TILDE);
    EXPECT_EQ(toks[2].type, TOK_DOT);
    EXPECT_EQ(toks[3].type, TOK_TILDE);
    EXPECT_EQ(toks[4].type, TOK_RPAREN);
    EXPECT_EQ(toks[5].type, TOK_IDENT);
    free(toks);
}

TEST(test_try_catch_finaly)
{
    /* TRY { } CATCH (e) { } FINALY { }
     * [0]=TRY [1]={ [2]=} [3]=CATCH [4]=( [5]=e [6]=) [7]={ [8]=} [9]=FINALY ... */
    u32 count;
    CandoToken *toks = lex_all("TRY { } CATCH (e) { } FINALY { }", &count);
    EXPECT_EQ(toks[0].type, TOK_TRY);
    EXPECT_EQ(toks[3].type, TOK_CATCH);
    EXPECT_EQ(toks[9].type, TOK_FINALY);
    free(toks);
}

TEST(test_class_snippet)
{
    /* CLASS Foo(x) { FUNCTION self.bar() { } }
     * [0]=CLASS [1]=Foo [2]=( [3]=x [4]=) [5]={ [6]=FUNCTION ... */
    u32 count;
    CandoToken *toks = lex_all("CLASS Foo(x) { FUNCTION self.bar() { } }", &count);
    EXPECT_EQ(toks[0].type, TOK_CLASS);
    EXPECT_EQ(toks[1].type, TOK_IDENT);   /* Foo */
    EXPECT_EQ(toks[6].type, TOK_FUNCTION);
    free(toks);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------ */
int main(void)
{
    printf("\n=== Cando lexer unit tests ===\n\n");

    printf("-- token names --\n");
    run_test("token type names",          test_token_type_names);

    printf("\n-- keyword lookup --\n");
    run_test("keyword lookup",            test_keyword_lookup);
    run_test("all keywords",              test_all_keywords);

    printf("\n-- lexer init / EOF --\n");
    run_test("empty source",              test_empty_source);
    run_test("whitespace only",           test_whitespace_only);

    printf("\n-- single-char tokens --\n");
    run_test("single-char tokens",        test_single_char_tokens);

    printf("\n-- multi-char operators --\n");
    run_test("multi-char operators",      test_multi_char_operators);

    printf("\n-- number literals --\n");
    run_test("integer literal",           test_integer_literal);
    run_test("float literal",             test_float_literal);
    run_test("number no leading dot",     test_number_no_leading_dot);
    run_test("multiple numbers",          test_multiple_numbers);

    printf("\n-- identifiers and keywords --\n");
    run_test("identifier",                test_identifier);
    run_test("underscore identifier",     test_underscore_ident);
    run_test("keyword IF",                test_keyword_if);
    run_test("keyword pipe (lower)",      test_keyword_pipe_lowercase);
    run_test("keywords in sequence",      test_keywords_in_sequence);

    printf("\n-- string literals --\n");
    run_test("double-quoted string",      test_string_double_quote);
    run_test("double-quoted escape",      test_string_double_quote_escape);
    run_test("single-quoted string",      test_string_single_quote);
    run_test("backtick string",           test_string_backtick);
    run_test("backtick nested braces",    test_string_backtick_nested_braces);
    run_test("unterminated double-quote", test_string_unterminated_dq);
    run_test("unterminated single-quote", test_string_unterminated_sq);
    run_test("unterminated backtick",     test_string_unterminated_bt);

    printf("\n-- comments --\n");
    run_test("line comment skipped",      test_line_comment_skipped);
    run_test("block comment skipped",     test_block_comment_skipped);
    run_test("unterminated block comment",test_unterminated_block_comment);

    printf("\n-- position tracking --\n");
    run_test("line tracking",             test_line_tracking);
    run_test("column tracking",           test_column_tracking);

    printf("\n-- peek --\n");
    run_test("peek does not advance",     test_peek_does_not_advance);

    printf("\n-- realistic snippets --\n");
    run_test("var declaration",           test_var_declaration);
    run_test("if statement",              test_if_statement);
    run_test("pipe operator",             test_pipe_operator);
    run_test("filter operator",           test_filter_operator);
    run_test("range operators",           test_range_operators);
    run_test("fluent call",               test_fluent_call);
    run_test("vararg",                    test_vararg);
    run_test("mask operators",            test_mask_operators);
    run_test("try/catch/finaly",          test_try_catch_finaly);
    run_test("class snippet",             test_class_snippet);

    printf("\n=== Results: %d/%d passed ===\n",
           g_tests_passed, g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
