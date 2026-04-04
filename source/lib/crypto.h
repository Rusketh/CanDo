/*
 * lib/crypto.h -- Cryptography and hashing standard library for Cando.
 *
 * Registers a global `crypto` object with:
 *
 *   crypto.md5(s)            → string
 *   crypto.sha256(s)         → string
 *   crypto.base64Encode(s)   → string
 *   crypto.base64Decode(s)   → string | null
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_CRYPTO_H
#define CANDO_LIB_CRYPTO_H

#include "../vm/vm.h"

void cando_lib_crypto_register(CandoVM *vm);

#endif /* CANDO_LIB_CRYPTO_H */
