# `_meta`

`_meta` is a writable global object that holds **meta tables** —
prototype objects for built-in types.  Native libraries register a
subtable per type and stamp every instance they create with
`instance.__index = _meta.<type>`, so user code can attach methods that
immediately become callable on every instance, present or future.

```cdo
print(type(_meta));                       // object
print(type(_meta.http_response));         // http_response

_meta.http_response.write = FUNCTION(self, data) {
    self.body = self.body + data;
};

http.createServer(FUNCTION(req, res) {
    res:write("Hello, world!");
    res:send();
}):listen(8080);
```

The `_meta.<name>` subtable's `__type` field is stamped immutably to
the type name, so `type(instance)` returns the type tag.  Method slots
(`status`, `send`, `listen`, …) use ordinary mutable flags so user
code may override them.

## Subtables registered by the standard library

| Subtable                        | Used by |
|---------------------------------|---------|
| `_meta.string`                  | Same table as the global `string` and the VM's string prototype.  Consulted whenever a method is called on a string. |
| `_meta.array`                   | Same table as `array`.  Consulted for methods on array receivers. |
| `_meta.object`                  | Same table as `object`.  Not auto-applied to plain objects; opt in with `object.setPrototype(o, _meta.object)`. |
| `_meta.thread`                  | Per-instance methods for thread receivers (`t:done()`, `t:join()`, `t:state()`, `t:then(fn)`, …).  Aliased onto the same native sentinels as `thread.<name>`. |
| `_meta.stream`                  | Methods on stream handles (`s:read`, `s:write`, `s:pipeTo`, …). |
| `_meta.http_request`            | Server-side request objects passed into `http.createServer`'s handler. |
| `_meta.http_response`           | Server-side response objects.  Default methods: `status`, `setHeader`, `send`, `json`. |
| `_meta.http_server`             | Server objects returned by `createServer`.  Default methods: `listen`, `close`. |
| `_meta.http_client_response`    | Response objects returned by `http.get`, `https.get`, `fetch`. |
| `_meta.tcp_socket`              | Plain-TCP connections (`socket` library).  See [socket.md](socket.md). |
| `_meta.tcp_server`              | TCP listeners. |
| `_meta.tls_socket`              | TLS connections (`secure_socket` library).  Inherits TCP methods plus three TLS introspection helpers. |
| `_meta.tls_server`              | TLS listeners. |

For `string`, `array`, `object`, and `thread` the meta table is the
**same underlying object** as the like-named global, so writing through
either name is observable through the other:

```cdo
_meta.string.shout = FUNCTION(self) { RETURN self:toUpper() + "!"; };
print("hi":shout());                  // HI!
print(string.shout("yes"));            // YES! — same table, same method
```

## Adding your own subtable

You may attach your own subtables at runtime and use them as prototypes
via `object.setPrototype`:

```cdo
_meta.point = {};
_meta.point.distance = FUNCTION(self, other) {
    VAR dx = self.x - other.x;
    VAR dy = self.y - other.y;
    RETURN math.sqrt(dx * dx + dy * dy);
};

VAR p = { x: 3, y: 4 };
object.setPrototype(p, _meta.point);
print(p:distance({ x: 0, y: 0 }));    // 5
```

This is the recommended way to grow methods onto user-defined types
when you don't want to use the [`CLASS`](../language/classes.md)
machinery.

## When to use `_meta` vs `CLASS`

- Use **`CLASS`** when you're modelling a user-level type with
  constructors, named instances, and an obvious "kind" name.
- Use **`_meta.<type>`** when you're extending an existing built-in
  type — for example adding `:writeLine` to every `tcp_socket` instance.

The two are interoperable: a `CLASS Foo`'s methods live on the class
object, which is itself reachable as `_meta.Foo` is **not**, however,
populated automatically.  Use `CLASS` for value types you control;
reach for `_meta.<type>` when you're amending a runtime-provided type.

## Caveats

- A poorly-chosen method name can collide with stdlib internals.
  Prefer specific verbs (`writeLine`, `formatJSON`) over generic ones
  (`process`, `handle`).
- Methods written on `_meta.<type>` are visible to **every** instance,
  including ones from other libraries that share the same handle type.
- `_meta` is a regular global object, so plain script code can corrupt
  it.  In production, treat it as write-once during initialization.
