# LDAP / Active Directory Module

A binary extension module that gives Cando scripts the same set of
operations exposed by the Windows PowerShell **ActiveDirectory** module:
read (search), write (add / modify / compare), move (rename /
modrdn), and delete — plus connection management (bind, unbind,
start_tls, set_option).

The module is portable C11 backed by:

| Platform | LDAP library | System package |
|---|---|---|
| Linux / macOS | OpenLDAP `libldap` | `libldap-dev` (Debian/Ubuntu), `openldap` (Homebrew) |
| Windows       | Windows native `wldap32.dll` | shipped with Windows SDK |

## Building

From the repository root:

```bash
make modules                                    # Linux / macOS host
make -C modules/ldap                            # build only this module
make -C modules/ldap test                       # run the C unit tests
```

For Windows, either build natively under MSYS2/MinGW, or cross-compile:

```bash
make -C modules/ldap ldap.dll MINGW_CC=x86_64-w64-mingw32-gcc
```

CI uploads the artefacts (`ldap.so` for Linux, `ldap.dll` for Windows)
from each workflow run.

## Usage

```cando
VAR ldap = include("./ldap.so");          // or "./ldap.dll" on Windows

// 1. Connect & authenticate
VAR conn = ldap.connect("ldap://dc.example.com:389");
ldap.set_option(conn, "protocol_version", 3);
ldap.set_option(conn, "referrals", FALSE);
ldap.bind(conn, "cn=admin,dc=example,dc=com", "secret");

// 2. Read (search)
VAR results = ldap.search(conn, {
    base:   "ou=Users,dc=example,dc=com",
    scope:  "sub",                         // base | one | sub
    filter: "(&(objectClass=user)(department=Eng))",
    attrs:  ["cn", "sAMAccountName", "mail"]
});
FOR entry OF results {
    print(entry.dn);
    print(entry.attributes.cn[0]);
}

// 3. Write (add a new object)
ldap.add(conn, "cn=Ada Lovelace,ou=Users,dc=example,dc=com", {
    objectClass:   ["top", "person", "organizationalPerson", "user"],
    cn:            "Ada Lovelace",
    sn:            "Lovelace",
    sAMAccountName:"alovelace",
    mail:          "ada@example.com"
});

// 4. Modify (replace, add, delete attribute values)
ldap.modify(conn, "cn=Ada Lovelace,ou=Users,dc=example,dc=com", [
    { op: "replace", attr: "title",       values: ["Engineer"] },
    { op: "add",     attr: "memberOf",    values: ["cn=Eng,ou=Groups,dc=example,dc=com"] },
    { op: "delete",  attr: "description"                                                  }
]);

// 5. Compare a single attribute value
VAR ok = ldap.compare(conn,
    "cn=Ada Lovelace,ou=Users,dc=example,dc=com",
    "title", "Engineer");

// 6. Move to a different OU
ldap.move(conn,
    "cn=Ada Lovelace,ou=Users,dc=example,dc=com",
    "ou=Disabled,dc=example,dc=com");

// 7. Rename (RDN change in place)
ldap.rename(conn,
    "cn=Ada Lovelace,ou=Disabled,dc=example,dc=com",
    "cn=Ada L");

// 8. Delete
ldap.delete(conn, "cn=Ada L,ou=Disabled,dc=example,dc=com");

// 9. Disconnect
ldap.unbind(conn);
```

## API reference

### `connect(uri) → connection`
Creates an LDAP session.  `uri` is a standard LDAP URL
(`ldap://host:port` or `ldaps://host:port`).  The connection is lazy:
no network round-trip happens until the first `bind` or other operation.

### `bind(conn, dn, password) → true`
Simple-bind authentication.  Pass empty strings for an anonymous bind,
or use `bind_anonymous(conn)`.

### `bind_anonymous(conn) → true`
Equivalent to `bind(conn, "", "")` but more explicit.

### `start_tls(conn) → true`
Upgrades the existing connection to TLS via the StartTLS extended
operation.  Call before `bind` for STARTTLS-then-bind flows.

### `set_option(conn, name, value) → true`
Mutates connection-level options.  Supported names:

| Name | Value | Notes |
|---|---|---|
| `protocol_version` | number (2 or 3) | Default 3.  Set before `bind`. |
| `referrals`        | boolean         | Whether to chase LDAP referrals. |
| `network_timeout`  | number (seconds, fractional ok) | Connect timeout. |
| `timelimit`        | number (seconds) | Server-side time limit per search. |
| `sizelimit`        | number          | Maximum search results per search. |

### `search(conn, options) → [entry, …]`
Performs a synchronous LDAP search.  `options` is an object:

```cando
{
    base:      "dc=example,dc=com",        // required
    scope:     "sub",                      // base | one | onelevel | sub | subtree
    filter:    "(objectClass=*)",          // RFC 4515 filter; default "(objectClass=*)"
    attrs:     ["cn", "mail"],             // null = return all attributes
    sizelimit: 1000,                       // 0 = server default
    timelimit: 30                          // 0 = server default
}
```

Each returned entry has shape:

```cando
{
    dn: "cn=Ada,...",
    attributes: {
        cn:   ["Ada Lovelace"],
        mail: ["ada@example.com"]
    }
}
```

Multi-valued attributes are returned as arrays even when a single value
is present.  Binary values are passed through as raw byte strings.

### `add(conn, dn, attrs) → true`
Creates a new directory object.  `attrs` is an object whose keys are
attribute names and whose values are either a single string or an
array of strings.

### `modify(conn, dn, mods) → true`
Applies one or more attribute modifications.  `mods` is an array of
objects:

```cando
[
    { op: "add",     attr: "memberOf", values: ["cn=Eng,..."] },
    { op: "replace", attr: "title",    values: ["Manager"]    },
    { op: "delete",  attr: "description"                      }
]
```

`values` is optional for the `delete` op (deletes the entire attribute).

### `compare(conn, dn, attr, value) → boolean`
RFC 4511 compare operation.  Returns `TRUE` if the value matches,
`FALSE` if it does not, throws on any other error.

### `delete(conn, dn) → true`
Removes a leaf object.

### `rename(conn, dn, new_rdn[, delete_old=true]) → true`
Renames an object in place (no parent change).

### `move(conn, dn, new_parent_dn[, new_rdn][, delete_old=true]) → true`
Moves an object under a new parent.  If `new_rdn` is omitted, the
object keeps its existing RDN.

### `unbind(conn) → true`
Closes the connection.  Subsequent operations on the handle throw.
Calling `unbind` on an already-closed handle is a no-op.

### `last_error() → { code, message } | null`
Returns the most recent LDAP error from this thread of execution, or
`null` if the last operation succeeded.  Useful for inspecting the
numeric LDAP result code (e.g. `LDAP_NO_SUCH_OBJECT`) when a `TRY`
block has caught the message.

### Constants

| Constant | Value |
|---|---|
| `VERSION`     | module version string (`"1.0.0"`) |
| `SCOPE_BASE`  | numeric LDAP_SCOPE_BASE |
| `SCOPE_ONE`   | numeric LDAP_SCOPE_ONELEVEL |
| `SCOPE_SUB`   | numeric LDAP_SCOPE_SUBTREE |

## Active Directory specifics

Microsoft Active Directory servers are LDAPv3 with a few quirks:

- Authentication usually uses *NT-style* names (`DOMAIN\user` or
  `user@domain.local`) rather than DNs.  Both work in `bind`.
- Some attributes (e.g. `unicodePwd`, `objectGUID`, `objectSid`) are
  binary; they will appear in `entry.attributes` as raw byte strings.
- AD requires that `userPassword` updates use the special `unicodePwd`
  attribute encoded as UTF-16LE wrapped in quotes; build that string
  yourself before passing it to `modify`.
- AD enforces TLS for password-changing operations — call
  `start_tls` after `connect`, or use `ldaps://`.

## Testing

The module ships with two layers of tests:

1. **C unit tests** (`test_ldap.c`) — exercise the pure-C helpers
   (scope-name parsing, modification op parsing, RDN extraction,
   `ldap_err2string` round-trip) without requiring the Cando VM.
   Run with `make -C modules/ldap test`.

2. **Script-level integration tests** (`test_ldap.cdo`) — load the
   compiled module through `include()` and verify the public API
   surface and every error path.  Run with
   `./cando modules/ldap/test_ldap.cdo`.

Operations that need a live directory server (search results, add /
modify / delete) are exercised against an intentionally unreachable
host so the error reporter is verified rather than masked.

## Limitations

- TLS certificate verification is left at OpenLDAP / wldap32 defaults.
  Set `TLS_REQCERT` in `/etc/ldap/ldap.conf` (Linux) or
  `LDAP_OPT_SERVER_CERTIFICATE` (Windows) before connecting if you
  need stricter checks.
- The module is synchronous only.  Asynchronous LDAP operations
  (`ldap_search_ext`, `ldap_result`) are not yet exposed.
- SASL mechanisms beyond `simple` (e.g. `GSSAPI`, `EXTERNAL`,
  `DIGEST-MD5`) are not yet exposed; use `bind` with a DN/password.
