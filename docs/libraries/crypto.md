# `crypto`

Hashes and base64 encoding.  Backed by OpenSSL when available, with
internal implementations as a fallback for the basic primitives.

> **Not for password storage.**  These primitives are appropriate for
> integrity checks and HTTP signing.  For password hashing use a slow,
> salted KDF — there is no built-in for that yet; call out via
> `process.spawn` to a tool like `argon2` if you need one today.

## Reference

### `crypto.md5(data) → string`

Lowercase hex MD5 digest of `data`.  `data` is a byte string.

```cdo
print(crypto.md5("hello"));        // 5d41402abc4b2a76b9719d911017c592
```

### `crypto.sha256(data) → string`

Lowercase hex SHA-256 digest.

```cdo
print(crypto.sha256("hello"));
// 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
```

### `crypto.base64Encode(data) → string`

Standard base64 (with `+` and `/`, padded with `=`).

```cdo
print(crypto.base64Encode("hello"));     // aGVsbG8=
```

### `crypto.base64Decode(text) → string | null`

Inverse.  Returns `NULL` on invalid input.

```cdo
print(crypto.base64Decode("aGVsbG8="));  // hello
print(crypto.base64Decode("not!base64")); // null
```

## Examples

### File integrity check

```cdo
FUNCTION sha256_file(path) {
    RETURN crypto.sha256(file.read(path, "binary"));
}

VAR expected = "2cf24dba…";
IF sha256_file("payload.bin") != expected {
    THROW "checksum mismatch";
}
```

### Basic-auth header

```cdo
FUNCTION basic_auth(user, pass) {
    RETURN "Basic " + crypto.base64Encode(user + ":" + pass);
}

http.get(`https://api/`, { headers: { Authorization: basic_auth("u", "p") } });
```

### Stable cache key from inputs

```cdo
FUNCTION cache_key(args) {
    RETURN crypto.sha256(json.stringify(args));
}
```
