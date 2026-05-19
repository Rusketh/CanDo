/*
 * lib/crypto.h -- Cryptography standard library for Cando.
 *
 * Registers a global `crypto` object with one-shot APIs for:
 *
 *   - Hashing (md5, sha1, sha224, sha256, sha384, sha512,
 *              sha3-{224,256,384,512}, blake2{b,s})
 *   - HMAC (any digest)
 *   - Key derivation (PBKDF2, scrypt, HKDF)
 *   - Random (randomBytes, randomInt, randomUUID)
 *   - Symmetric ciphers (AES-{ECB,CBC,CTR,GCM,CCM} all key sizes,
 *                         ChaCha20, ChaCha20-Poly1305)
 *   - Asymmetric: generateKeyPair / sign / verify /
 *                  publicEncrypt / privateDecrypt /
 *                  diffieHellman   (RSA / EC / Ed25519 / X25519)
 *   - X.509:  crypto.x509.{create,parse,verify,fingerprint,csr}
 *   - Encoding helpers: crypto.hex.*, crypto.base64.*,
 *                        crypto.base64url.*, crypto.timingSafeEqual
 *
 * All hash/HMAC/KDF/cipher outputs accept an optional trailing
 * `encoding` string ("hex" (default), "base64", "base64url", "bytes")
 * that controls how the binary output is returned to the script.
 *
 * Backed by OpenSSL (-lcrypto), which the rest of the runtime already
 * links for HTTPS / secure sockets.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_CRYPTO_H
#define CANDO_LIB_CRYPTO_H

#include "../vm/vm.h"

CANDO_API void cando_lib_crypto_register(CandoVM *vm);

#endif /* CANDO_LIB_CRYPTO_H */
