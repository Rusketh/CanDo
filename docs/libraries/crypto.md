# `crypto`

Comprehensive cryptography backed by OpenSSL.  Provides one-shot APIs
across the surface Node.js's `node:crypto` covers: hashing, HMAC, key
derivation, random, symmetric ciphers (AES, ChaCha20-Poly1305),
asymmetric (RSA, EC, Ed25519, X25519), and X.509.

## Encoding

Every routine that produces or accepts bytes takes a trailing
**encoding** argument (when an output) or interprets its input bytes
as the named encoding.  Supported values:

| Encoding | Notes |
|----------|-------|
| `"hex"`        | Lowercase hex; the default for hash / HMAC. |
| `"base64"`     | RFC 4648 with `+/` and `=` padding. |
| `"base64url"`  | RFC 4648 §5 url-safe, no padding. |
| `"bytes"`      | Raw binary string; default for KDFs, ciphers, signatures, DH. |

Aliases: `"binary"` and `"raw"` map to `"bytes"`.

## Hashing

| Function | Output |
|----------|--------|
| `crypto.md5(data, enc?)`      | 16-byte MD5 |
| `crypto.sha1(data, enc?)`     | 20-byte SHA-1 |
| `crypto.sha224(data, enc?)`   | 28-byte SHA-224 |
| `crypto.sha256(data, enc?)`   | 32-byte SHA-256 |
| `crypto.sha384(data, enc?)`   | 48-byte SHA-384 |
| `crypto.sha512(data, enc?)`   | 64-byte SHA-512 |
| `crypto.sha3_224(data, enc?)` | 28-byte SHA3-224 |
| `crypto.sha3_256(data, enc?)` | 32-byte SHA3-256 |
| `crypto.sha3_384(data, enc?)` | 48-byte SHA3-384 |
| `crypto.sha3_512(data, enc?)` | 64-byte SHA3-512 |
| `crypto.blake2b(data, enc?)`  | 64-byte BLAKE2b |
| `crypto.blake2s(data, enc?)`  | 32-byte BLAKE2s |
| `crypto.hash(algo, data, enc?)` | Dynamic-dispatch hash by algo name |

```cdo
print(crypto.sha256("hello"));
// 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824

print(crypto.sha256("hello", "base64"));
// LPJNul+wow4m6DsqxbninhsWHlwfp0JecwQzYpOLmCQ=
```

## HMAC

```
crypto.hmac(algo, key, data, enc?) → string
```

`algo` is any digest name (`"sha256"`, `"sha512"`, ...).

```cdo
print(crypto.hmac("sha256", "key",
        "The quick brown fox jumps over the lazy dog"));
// f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
```

## Key Derivation

```
crypto.pbkdf2(password, salt, iterations, keylen, digest, enc?)
crypto.scrypt(password, salt, keylen, N?, r?, p?, enc?)
crypto.hkdf(digest, ikm, salt, info, keylen, enc?)
```

```cdo
var key = crypto.pbkdf2("password", "salt", 100000, 32, "sha256");
```

## Random

| Function | Returns |
|----------|---------|
| `crypto.randomBytes(n, enc?)` | `n` random bytes (default encoding `"bytes"`). |
| `crypto.randomInt(min, max)`  | Integer in `[min, max)`. |
| `crypto.randomInt(max)`       | Integer in `[0, max)`. |
| `crypto.randomUUID()`         | RFC 4122 v4 UUID string. |

## Symmetric Ciphers

```
crypto.encrypt(algo, key, iv, plaintext, aad?, enc?)
crypto.decrypt(algo, key, iv, ciphertext, aad?, tag?, enc?)
```

Supported `algo`:

- `"aes-128-gcm"`, `"aes-192-gcm"`, `"aes-256-gcm"`
- `"aes-128-cbc"`, `"aes-192-cbc"`, `"aes-256-cbc"`
- `"aes-128-ctr"`, `"aes-192-ctr"`, `"aes-256-ctr"`
- `"aes-128-ecb"`, `"aes-192-ecb"`, `"aes-256-ecb"`
- `"chacha20"`, `"chacha20-poly1305"`

For **AEAD** modes (`-gcm`, `chacha20-poly1305`), `encrypt` returns
`{ ciphertext, tag }` and `decrypt` requires the `tag` argument.
Authentication failure throws.

For non-AEAD modes, `encrypt` returns the ciphertext bytes (or the
requested encoding) and `decrypt` returns the plaintext.

```cdo
var key = crypto.randomBytes(32);
var iv  = crypto.randomBytes(12);
var ct  = crypto.encrypt("aes-256-gcm", key, iv, "secret", "aad");
var pt  = crypto.decrypt("aes-256-gcm", key, iv,
                         ct.ciphertext, "aad", ct.tag);
print(pt);   // "secret"
```

## Asymmetric

### Key generation

```
crypto.generateKeyPair(type, opts?) → { publicKey, privateKey }
```

- `type ∈ { "rsa", "ec", "ed25519", "x25519" }`.
- RSA `opts`: `{ modulusLength: 2048 | 3072 | 4096 }` (default 2048).
- EC `opts`: `{ curve: "P-256" | "P-384" | "P-521" | "secp256k1" }`
  (default `"P-256"`).

Keys are returned as PEM strings.

### Sign / verify

```
crypto.sign(privateKeyPem, data, opts?, enc?) → signature bytes
crypto.verify(publicKeyPem, data, signature, opts?) → bool
```

- Default `opts.digest`: `"sha256"`.  Ignored for Ed25519.
- RSA `opts.padding`: `"pkcs1"` (default) or `"pss"`; `opts.saltLength`
  for PSS.

### RSA encryption

```
crypto.publicEncrypt(publicKeyPem, data, opts?, enc?)
crypto.privateDecrypt(privateKeyPem, data, opts?, enc?)
```

- `opts.padding`: `"oaep"` (default), `"pkcs1"`, or `"none"`.

### Diffie-Hellman (X25519, ECDH)

```
crypto.diffieHellman(privateKeyPem, peerPublicKeyPem, enc?) → shared secret bytes
```

### Example: Ed25519 round-trip

```cdo
var kp  = crypto.generateKeyPair("ed25519");
var sig = crypto.sign(kp.privateKey, "hello");
print(crypto.verify(kp.publicKey, "hello", sig));    // true
print(crypto.verify(kp.publicKey, "tampered", sig)); // false
```

## X.509

```
crypto.x509.create(subject, opts) → pem
crypto.x509.parse(pem) → { subject, issuer, notBefore, notAfter,
                            serialNumber, fingerprint, publicKey }
crypto.x509.verify(certPem, caPem) → bool
crypto.x509.fingerprint(pem, algo?) → hex string  (default sha256)
crypto.x509.csr(privateKeyPem, subject) → pem
```

`subject` / `issuer` are objects with field-shaped keys:
`{ CN, O, OU, C, ST, L, emailAddress }`.

```cdo
var kp = crypto.generateKeyPair("rsa", { modulusLength: 2048 });
var cert = crypto.x509.create(
    { CN: "test.example", O: "Acme", C: "US" },
    { privateKey: kp.privateKey, days: 365 }
);
var parsed = crypto.x509.parse(cert);
print(parsed.subject.CN);   // "test.example"
```

## Encoding helpers

```
crypto.hex.encode(data) / crypto.hex.decode(data)
crypto.base64.encode(data) / crypto.base64.decode(data)
crypto.base64url.encode(data) / crypto.base64url.decode(data)
```

Legacy aliases `crypto.base64Encode` and `crypto.base64Decode` remain
for back-compat.

## Constant-time comparison

```
crypto.timingSafeEqual(a, b) → bool
```

Constant-time bytewise compare via OpenSSL's `CRYPTO_memcmp`.
