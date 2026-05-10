# `ldap` module

LDAP / Active Directory client.  Built on OpenLDAP's `libldap`.  Covers
authentication (simple bind, SASL), search, and the standard set of
modify operations.  Includes helpers for AD-specific concerns like
`unicodePwd` encoding, group expansion, and reversible-password
decoding.

## Loading

```cdo
VAR ldap = include("./modules/ldap/ldap");
```

Requires `libldap.so` at runtime.

## Connecting

### `ldap.connect(uri, opts*) → conn`

`uri` is one or more `ldap://` / `ldaps://` URIs (space-separated for
fail-over).  `opts`:

| Field         | Type   | Default | Description |
|---------------|--------|---------|-------------|
| `version`     | number | `3`     | LDAP protocol version. |
| `timeout`     | number | `0`     | Network timeout (seconds). |
| `referrals`   | bool   | `FALSE` | Follow referrals automatically. |
| `tls`         | object | —       | `{ ca, cert, key, verify }` for `ldaps://` or `start_tls`. |

```cdo
VAR c = ldap.connect("ldaps://dc.corp.example.com", {
    timeout: 5,
    tls: { ca: file.read("ca.pem") },
});
```

## Authentication

### `c:bind(dn, password)`

Simple bind.  Throws on failure with the LDAP error message.

### `c:bind_anonymous()`

Anonymous bind.

### `c:start_tls()`

Upgrade an `ldap://` connection to TLS.  Use before `bind`.

### `c:unbind()` / `c:close()`

Close the connection.  Idempotent.

## Search

### `c:search(base, filter, opts*) → array`

Search the directory.

`opts`:

| Field       | Type   | Default     | Description |
|-------------|--------|-------------|-------------|
| `scope`     | string | `"subtree"` | `"base"`, `"one"`, or `"subtree"`. |
| `attrs`     | array  | all         | Attributes to return. |
| `sizeLimit` | number | `0`         | 0 = unlimited. |
| `timeLimit` | number | `0`         | Server-side time limit. |

```cdo
VAR users = c:search("DC=corp,DC=example,DC=com",
    "(&(objectClass=user)(sAMAccountName=alice))");
print(users[0].cn);
print(users[0].memberOf);     // array of DNs
```

Result rows are objects whose keys are attribute names and whose values
are either single strings or arrays of strings (LDAP multi-valued
attributes).

## Modify

| Method                                | Description |
|---------------------------------------|-------------|
| `c:add(dn, attrs)`                    | Create a new entry. |
| `c:delete(dn)`                        | Delete an entry. |
| `c:modify(dn, ops)`                   | Apply a list of `{ op, attr, values }` operations. |
| `c:rename(dn, newRdn, parent*)`       | Move/rename an entry. |
| `c:move(dn, newParent)`               | Convenience around `rename`. |
| `c:compare(dn, attr, value) → bool`   | Server-side attribute compare. |

```cdo
c:modify("CN=Alice,OU=People,DC=corp,DC=example,DC=com", [
    { op: "replace", attr: "title",       values: ["Director"] },
    { op: "add",     attr: "memberOf",    values: ["CN=Admins,OU=Groups,..."] },
]);
```

## Active Directory helpers

| Method                                            | Description |
|---------------------------------------------------|-------------|
| `c:password_modify(dn, newPassword)`              | Use the modern Password Modify Extended Operation. |
| `c:test_credentials(dn, password) → bool`         | Bind in a side connection and report success. |
| `c:members(groupDn) → array`                      | Walk `member` recursively, returning user DNs. |
| `c:member_of(userDn) → array`                     | All groups (direct + transitive) the user belongs to. |
| `ldap.escape_dn(s) → string`                      | Escape a DN component. |
| `ldap.escape_filter(s) → string`                  | Escape a filter value. |
| `ldap.decode_reversible_password(blob, key)`      | Decode AD's reversible-password format. |
| `ldap.md5(data)` / `ldap.rc4(key, data)`          | Helpers used by AD encoding routines. |

## Server discovery

### `c:rootdse() → object`

Read the server's root DSE — schema namingContexts, supported
extensions, server name, etc.

### `c:set_option(name, value)`

Pass through to `ldap_set_option`.  Recognized names include
`"size_limit"`, `"time_limit"`, `"network_timeout"`, `"referrals"`.

## Examples

### Authenticate a user

```cdo
TRY {
    VAR c = ldap.connect("ldaps://dc.corp.example.com");
    c:bind(`uid=${user},ou=People,dc=corp,dc=example,dc=com`, password);
    c:close();
    RETURN TRUE;
} CATCH (e) {
    log(`auth failed: ${e}`);
    RETURN FALSE;
}
```

### List a group's transitive members

```cdo
VAR c = ldap.connect("ldap://dc.corp.example.com");
c:bind(SERVICE_DN, SERVICE_PW);

VAR users = c:members("CN=AllStaff,OU=Groups,DC=corp,DC=example,DC=com");
print(`${#users} users`);

c:close();
```

### Bulk attribute update

```cdo
VAR conn = ldap.connect(LDAP_URI);
conn:bind(SERVICE_DN, SERVICE_PW);

FOR row OF csv.parse(file.read("changes.csv")) {
    conn:modify(row.dn, [
        { op: "replace", attr: "title", values: [row.title] },
    ]);
}

conn:close();
```

## Errors

Every method throws on LDAP failure with the OpenLDAP error message
as the thrown value.  Wrap calls in `TRY` / `CATCH` to handle.
