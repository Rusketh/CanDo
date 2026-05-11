# `net`

DNS lookups.

For TCP / TLS connections, see [socket.md](socket.md) and
[secure_socket.md](secure_socket.md) — both libraries do their own
resolution internally and accept hostnames directly.

## Reference

### `net.lookup(hostname) → array | null`

Resolve a hostname to an array of IPv4 address strings, or `NULL` if
resolution fails.  Every address returned by the host resolver is
included.

```cdo
print(inspect(net.lookup("example.com")));   // ["93.184.216.34", ...]
print(net.lookup("nope.invalid"));           // null
```

For IPv6 results, use [`socket.resolve`](socket.md) instead — it
returns up to 16 numeric addresses across both address families.
