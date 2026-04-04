/*
 * lexer.h -- Cando source lexer public API.
 *
 * The lexer converts raw UTF-8 source text into a stream of CandoTokens.
 * It is a single-pass, hand-written scanner; the caller drives it by
 * repeatedly calling cando_lexer_next() until TOK_EOF or TOK_ERROR.
 *
 * The lexer does NOT own the source buffer.  The caller must keep the
 * buffer alive for the duration of lexing (tokens hold pointers into it).
 *
 * Usage:
 *   CandoLexer lex;
 *   cando_lexer_init(&lex, src, src_len);
 *   CandoToken tok;
 *   do {
 *       tok = cando_lexer_next(&lex);
 *   } while (tok.type != TOK_EOF && tok.type != TOK_ERROR);
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LEXER_H
#define CANDO_LEXER_H

#include "../core/common.h"
#include "token.h"

/* -------------------------------------------------------------------------
 * CandoLexer -- mutable scanner state.
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *source;       /* start of the source buffer (not owned)    */
    usize       source_len;   /* total byte count of the buffer            */
    usize       pos;          /* current scan cursor (byte index)          */
    u32         line;         /* current 1-based line number               */
    usize       line_start;   /* byte offset of the start of current line  */
    char        error_msg[256]; /* populated when cando_lexer_next returns  */
                                /* TOK_ERROR                                */
} CandoLexer;

/* -------------------------------------------------------------------------
 * cando_lexer_init -- initialise a lexer for the given source buffer.
 *
 * `source`     -- pointer to the first byte of the source text.
 * `source_len` -- number of bytes in the buffer (NUL not required).
 * ------------------------------------------------------------------------ */
void cando_lexer_init(CandoLexer *lex, const char *source, usize source_len);

/* -------------------------------------------------------------------------
 * cando_lexer_next -- scan and return the next token.
 *
 * Whitespace and comments (// line  and  slash-star block) are skipped silently.
 * Returns TOK_EOF at end of input.
 * Returns TOK_ERROR on unrecognised input; lex->error_msg describes the
 * problem.
 * ------------------------------------------------------------------------ */
CandoToken cando_lexer_next(CandoLexer *lex);

/* -------------------------------------------------------------------------
 * cando_lexer_peek -- return the next token WITHOUT advancing the lexer.
 *
 * Saves and restores all mutable lexer state.  Useful for one-token
 * lookahead in the parser without a separate token buffer.
 * ------------------------------------------------------------------------ */
CandoToken cando_lexer_peek(CandoLexer *lex);

/* -------------------------------------------------------------------------
 * cando_lexer_error_msg -- return the error message from the last
 * TOK_ERROR token, or NULL if the last token was not an error.
 * ------------------------------------------------------------------------ */
const char *cando_lexer_error_msg(const CandoLexer *lex);

#endif /* CANDO_LEXER_H */
