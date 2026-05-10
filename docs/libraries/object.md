# `object`

The `object` namespace contains utilities for inspecting and
manipulating objects: shallow copies, key/value access without
metamethod dispatch, prototype management, and a re-entrant lock for
cross-thread coordination.

All functions take the object as the first argument.  The prototype
chain (`__index`) is **not** consulted unless explicitly noted.

## Reference

### `object.keys(o) → array`

Array of own keys, in FIFO insertion order.

```cdo
print(inspect(object.keys({ a: 1, b: 2 })));    // ["a", "b"]
```

### `object.values(o) → array`

Array of own values, in FIFO insertion order.

```cdo
print(inspect(object.values({ a: 1, b: 2 })));   // [1, 2]
```

### `object.get(o, key) → any`

Raw read.  Returns `o[key]` without invoking `__index`.

```cdo
CLASS Animal = (self) { }
Animal.species = "mystery";

VAR cat = Animal();
print(cat.species);                 // mystery — via __index
print(object.get(cat, "species"));  // null    — own field only
```

### `object.set(o, key, value) → bool`

Raw write.  Returns `TRUE` on success, `FALSE` on a static-flagged
field.  Skips `__newindex`.

```cdo
object.set(o, "x", 42);
```

### `object.copy(o) → object`

Shallow copy of `o`'s own fields.  The prototype is *not* copied.

```cdo
VAR src = { a: 1, b: [2, 3] };
VAR dst = object.copy(src);
dst.a = 99;
print(src.a);                       // 1
dst.b:push(4);
print(inspect(src.b));              // [2, 3, 4]   — array shared
```

### `object.assign(o, ...sources) → object`

Mutating merge.  Copies every own field from each source onto `o` (in
order; later sources win).  Returns `o`.

```cdo
VAR target = { a: 1 };
object.assign(target, { b: 2 }, { c: 3, a: 99 });
print(inspect(target));             // { a: 99, b: 2, c: 3 }
```

### `object.apply(o, ...sources) → object`

Like `assign`, but produces a **new** object instead of mutating `o`.

```cdo
VAR base   = { theme: "dark", lang: "en" };
VAR custom = object.apply(base, { lang: "fr" });
print(inspect(base));               // { theme: "dark", lang: "en" }
print(inspect(custom));             // { theme: "dark", lang: "fr" }
```

### `object.getPrototype(o) → object | null`

Return `o.__index` if it points at an object, else `NULL`.

### `object.setPrototype(o, proto) → object`

Set `__index` on `o`.  `proto = NULL` removes it.  Returns `o`.

```cdo
VAR base = { greet: FUNCTION(self) { RETURN `hi ${self.name}`; } };
VAR a = { name: "Alice" };
object.setPrototype(a, base);
print(a:greet());                   // hi Alice
```

### `object.lock(o) → object`

Acquire `o`'s **re-entrant script lock**.  Returns `o`.  Blocks if
another thread holds the lock.

The same thread can call `lock` multiple times on the same object as
long as it later calls `unlock` the same number of times.

### `object.unlock(o)`

Release one level of `o`'s lock.

### `object.locked(o) → bool`

`TRUE` if any thread holds the lock on `o`.  Useful for diagnostics
only — there is an inherent race between the check and any subsequent
acquisition.

```cdo
VAR shared = { count: 0 };

FUNCTION bump() {
    object.lock(shared);
    TRY {
        shared.count = shared.count + 1;
    } FINALY {
        object.unlock(shared);
    }
}

VAR ts = [];
FOR i IN 1 -> 100 { ts:push(thread bump()); }
FOR t OF ts { await t; }
print(shared.count);                // 100
```

## Examples

### Frozen-by-convention defaults

```cdo
CONST DEFAULTS = { port: 8080, host: "localhost", workers: 4 };

FUNCTION configure(overrides) {
    RETURN object.apply(DEFAULTS, overrides || {});
}

VAR cfg = configure({ port: 9000 });
print(inspect(cfg));                // { port: 9000, host: "localhost", workers: 4 }
print(inspect(DEFAULTS));           // unchanged
```

### Walking a prototype chain

```cdo
FUNCTION ancestors(instance) {
    VAR out = [];
    VAR p = object.getPrototype(instance);
    WHILE p != NULL {
        out:push(p);
        p = object.getPrototype(p);
    }
    RETURN out;
}
```
