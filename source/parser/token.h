/*
 * token.h -- Token type definitions for the Cando lexer.
 *
 * Defines all token types recognised by the Cando language, the CandoToken
 * struct, and a keyword-lookup helper.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_TOKEN_H
#define CANDO_TOKEN_H

#include "../core/common.h"

/* -------------------------------------------------------------------------
 * TokenType -- discriminant for every lexeme the scanner can produce.
 *
 * Ordering within groups is arbitrary; do not rely on numeric values.
 * ------------------------------------------------------------------------ */
typedef enum {

    /* --- Literals -------------------------------------------------------- */
    TOK_NUMBER,          /* integer or floating-point literal               */
    TOK_STRING_DQ,       /* double-quoted string  "..."  (escape sequences) */
    TOK_STRING_SQ,       /* single-quoted string  '...'  (multiline)        */
    TOK_STRING_BT,       /* backtick string  `...`  (interpolation ${EXPR}) */
    TOK_IDENT,           /* identifier (not a keyword)                      */

    /* --- Keywords --------------------------------------------------------
     * Every entry here has a matching entry in the keyword table inside
     * token.c.  The lexer converts TOK_IDENT into the appropriate keyword
     * token after matching the lexeme against that table.
     * -------------------------------------------------------------------- */
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_FUNCTION,
    TOK_CLASS,
    TOK_RETURN,
    TOK_THROW,
    TOK_TRY,
    TOK_CATCH,
    TOK_FINALY,          /* note: spec spells it FINALY (one L)             */
    TOK_CONST,
    TOK_VAR,
    TOK_GLOBAL,
    TOK_STATIC,
    TOK_PRIVATE,
    TOK_ASYNC,
    TOK_AWAIT,
    TOK_THREAD,          /* 'thread' -- spawn expression as OS thread       */
    TOK_NULL_KW,         /* NULL keyword                                    */
    TOK_TRUE_KW,         /* TRUE keyword                                    */
    TOK_FALSE_KW,        /* FALSE keyword                                   */
    TOK_IN,
    TOK_OF,
    TOK_OVER,
    TOK_CONTINUE,
    TOK_BREAK,
    TOK_PIPE_KW,         /* 'pipe' -- implicit iteration variable in ~>/~!> */

    /* --- Multi-character operators --------------------------------------- */
    TOK_PIPE_OP,         /* ~>   functional map (pipe)                      */
    TOK_FILTER_OP,       /* ~!>  functional filter                          */
    TOK_RANGE_ASC,       /* ->   ascending range generator                  */
    TOK_RANGE_DESC,      /* <-   descending range generator                 */
    TOK_FLUENT,          /* ::   fluent method call (returns receiver)      */
    TOK_VARARG,          /* ...  vararg / unpack                            */
    TOK_FAT_ARROW,       /* =>   lambda arrow (shorthand function)          */
    TOK_AND,             /* &&   logical AND                                */
    TOK_OR,              /* ||   logical OR                                 */
    TOK_EQ,              /* ==   equality                                   */
    TOK_NEQ,             /* !=   inequality                                 */
    TOK_LEQ,             /* <=   less-than-or-equal                         */
    TOK_GEQ,             /* >=   greater-than-or-equal                      */
    TOK_LSHIFT,          /* <<   bitwise left shift                         */
    TOK_RSHIFT,          /* >>   bitwise right shift                        */
    TOK_BITXOR,          /* |&   bitwise XOR                                */
    TOK_PLUS_ASSIGN,     /* +=                                              */
    TOK_MINUS_ASSIGN,    /* -=                                              */
    TOK_STAR_ASSIGN,     /* *=                                              */
    TOK_SLASH_ASSIGN,    /* /=                                              */
    TOK_PERCENT_ASSIGN,  /* %=                                              */
    TOK_CARET_ASSIGN,    /* ^=                                              */
    TOK_INCR,            /* ++   increment                                  */
    TOK_DECR,            /* --   decrement                                  */

    /* --- Single-character operators / punctuation ----------------------- */
    TOK_PLUS,            /* +  */
    TOK_MINUS,           /* -  */
    TOK_STAR,            /* *  */
    TOK_SLASH,           /* /  */
    TOK_PERCENT,         /* %  */
    TOK_CARET,           /* ^  (power)                                      */
    TOK_AMP,             /* &  (bitwise AND)                                */
    TOK_BITOR,           /* |  (bitwise OR)                                 */
    TOK_LT,              /* <                                               */
    TOK_GT,              /* >                                               */
    TOK_ASSIGN,          /* =                                               */
    TOK_BANG,            /* !                                               */
    TOK_TILDE,           /* ~  (mask / selector prefix)                     */
    TOK_DOT,             /* .  (field access / mask skip)                   */
    TOK_HASH,            /* #  (length prefix operator)                     */
    TOK_LPAREN,          /* (  */
    TOK_RPAREN,          /* )  */
    TOK_LBRACE,          /* {  */
    TOK_RBRACE,          /* }  */
    TOK_LBRACKET,        /* [  */
    TOK_RBRACKET,        /* ]  */
    TOK_SEMI,            /* ;  */
    TOK_COMMA,           /* ,  */
    TOK_COLON,           /* :  (method call or object key separator)        */

    /* --- Sentinel / error ----------------------------------------------- */
    TOK_EOF,             /* end of source                                   */
    TOK_ERROR,           /* unrecognised input; message in CandoLexer       */

    TOK_COUNT            /* number of distinct token types (keep last)      */
} CandoTokenType;

/* -------------------------------------------------------------------------
 * CandoToken -- a single lexed token.
 *
 * `start` points into the caller-owned source buffer and is valid as long as
 * that buffer lives.  The token does NOT copy the lexeme.
 * ------------------------------------------------------------------------ */
typedef struct {
    CandoTokenType   type;
    const char *start;   /* pointer to the first byte of the lexeme        */
    u32         length;  /* byte count of the lexeme                       */
    u32         line;    /* 1-based line number of the first character      */
    u32         col;     /* 1-based column of the first character           */
} CandoToken;

/* -------------------------------------------------------------------------
 * cando_token_type_name -- return a static ASCII name for a TokenType.
 * Useful for diagnostics and test assertions.
 * ------------------------------------------------------------------------ */
const char *cando_token_type_name(CandoTokenType t);

/* -------------------------------------------------------------------------
 * cando_keyword_type -- if the given identifier lexeme (length `len`) is a
 * reserved keyword, return its keyword token type; otherwise return TOK_IDENT.
 *
 * The comparison is case-sensitive: Cando keywords are all upper-case
 * ("IF", "WHILE", ...) except for "pipe" which is lower-case.
 * ------------------------------------------------------------------------ */
CandoTokenType cando_keyword_type(const char *ident, u32 len);

#endif /* CANDO_TOKEN_H */
