# `net`

DNS lookups.

For TCP / TLS connections, see [socket.md](socket.md) and
[secure_socket.md](secure_socket.md) — both libraries do their own
resolution internally and accept hostnames directly.

## Reference

### `net.lookup(hostname) → string | null`

Resolve a hostname to an IPv4 address string, or `NULL` if resolution
fails.

```cdo
print(net.lookup("example.com"));     // "93.184.216.34"
print(net.lookup("nope.invalid"));    // null
```

For multiple addresses or IPv6 results, use [`socket.resolve`](socket.md)
instead — it returns an array of up to 16 numeric addresses (IPv4 and
IPv6).
