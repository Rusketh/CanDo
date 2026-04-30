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

## Error handling

Every operation that can fail throws a CanDo error.  Catch with
multi-value `CATCH` to inspect both the human-readable message and the
numeric LDAP result code:

```cando
TRY {
    ldap.bind(conn, dn, pw);
} CATCH (msg, code, diag) {
    IF code == 49 { print("invalid credentials"); }
    IF code == 81 { print("server is down"); }
    print(msg);   // formatted module-side message
    print(diag);  // libldap diagnostic (POSIX only; "" on Windows)
}
```

| Code | Constant            | Meaning |
|------|---------------------|---------|
| 0    | LDAP_SUCCESS        | (used for module-side errors with no LDAP result code) |
| 32   | LDAP_NO_SUCH_OBJECT | DN does not exist |
| 49   | LDAP_INVALID_CREDENTIALS | bind: wrong password |
| 50   | LDAP_INSUFFICIENT_ACCESS | caller lacks rights for the operation |
| 53   | LDAP_UNWILLING_TO_PERFORM | server policy refused (e.g. password too short) |
| 68   | LDAP_ALREADY_EXISTS | add: target DN already exists |
| 81   | LDAP_SERVER_DOWN    | network failure / server unreachable |

`code` is `0` for module-internal errors (bad arguments, out of memory,
unknown option name).  `diag` may be `""` even on POSIX when libldap has
nothing to report.

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
Mutates connection- or library-level options.  Supported names:

| Name | Value | Notes |
|---|---|---|
| `protocol_version` | number (2 or 3) | Default 3.  Set before `bind`. |
| `referrals`        | boolean         | Whether to chase LDAP referrals. |
| `network_timeout`  | number (seconds, fractional ok) | Connect timeout. |
| `timelimit`        | number (seconds) | Server-side time limit per search. |
| `sizelimit`        | number          | Maximum search results per search. |
| `tls_cacertfile`   | string (path)   | CA bundle for verifying the server cert.  POSIX only — Schannel reads the Windows trust store. |
| `tls_certfile`     | string (path)   | Client cert for SASL EXTERNAL.  POSIX only. |
| `tls_keyfile`      | string (path)   | Private key matching `tls_certfile`.  POSIX only. |
| `tls_require_cert` | `"never" \| "allow" \| "try" \| "demand"` | Cert validation strictness.  On Windows, `"never"` disables SSL validation, all other values enable it. |

### `search(conn, options) → [entry, …]`
Performs a synchronous LDAP search.  `options` is an object:

```cando
{
    base:       "dc=example,dc=com",        // required
    scope:      "sub",                      // base | one | onelevel | sub | subtree
    filter:     "(objectClass=*)",          // RFC 4515 filter; default "(objectClass=*)"
    attrs:      ["cn", "mail"],             // null = return all attributes
    sizelimit:  1000,                       // 0 = server default
    timelimit:  30,                         // 0 = server default
    page_size:  0                           // > 0 enables RFC 2696 paged results
}
```

When `page_size > 0` the module attaches a paged-results control and
loops internally until the server reports no more pages, accumulating
all entries into the single returned array.  This is the only way to
retrieve more than 1000 entries from a default-configured Active
Directory.

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
Closes the connection and releases its slot in the connection pool.
Subsequent operations on the handle throw.  Calling `unbind` on an
already-closed handle is a no-op.

### `rootdse(conn[, attrs]) → attributes-object | null`
Reads the directory's rootDSE — the per-server status entry that lists
`namingContexts`, `supportedControl`, `supportedExtension`,
`defaultNamingContext` (AD), `vendorName`, etc.  Returns the same
shape as `entry.attributes` from `search`.

### `test_credentials(uri, dn, password) → bool`
One-shot credential validation.  Opens a fresh connection, performs a
simple bind, unbinds, and returns `TRUE` on success or `FALSE` on
`LDAP_INVALID_CREDENTIALS`.  Any other failure (server unreachable,
TLS failure, etc.) re-throws so genuine errors aren't silently coerced
into "wrong password".  Useful for password-prompt screens.

### `password_modify(conn, dn, old_password, new_password[, format]) → true`
Set a user's password.

| `format` | Action |
|---|---|
| `"auto"` (default) | Picks AD vs RFC 3062 from rootDSE / platform; defaults to AD on Windows. |
| `"ad"`             | Active Directory style: `modify` on `unicodePwd` with `'"'+UTF-16LE+'"'`. AD requires LDAPS or StartTLS for this to succeed. |
| `"rfc3062"`        | LDAP extended op `1.3.6.1.4.1.4203.1.11.1`.  POSIX only; throws on Windows. |

Pass `""` for `old_password` for an admin-mode reset (the bound
principal must have password-reset rights).

### `members(conn, group_dn[, options]) → [dn, ...]`
Return the DNs of users that are members of `group_dn`.
- `options.recursive: TRUE` walks nested groups transitively.  On
  Active Directory this uses the `LDAP_MATCHING_RULE_IN_CHAIN`
  matching rule (OID `1.2.840.113556.1.4.1941`) for a single
  server-side query; on plain OpenLDAP it falls back to a
  client-side breadth-first walk.

### `member_of(conn, user_dn[, options]) → [dn, ...]`
Return the DNs of groups that `user_dn` is a member of.  Uses the
user's `memberOf` attribute (virtual on AD, requires the `memberof`
overlay on OpenLDAP).
- `options.recursive: TRUE` resolves transitive group nesting via the
  same AD matching rule / BFS fallback as `members`.

### `compare(conn, dn, attr, value) → boolean`
RFC 4511 compare.  Returns `TRUE` if equal, `FALSE` if not, throws on
any other error.

### `escape_filter(value) → string`  (RFC 4515)
Escape a single string for safe interpolation into a search filter:

```cando
VAR f = `(&(objectClass=user)(cn=${ldap.escape_filter(name)}))`;
ldap.search(conn, { base: "dc=ex", filter: f });
```

### `escape_dn(value) → string`  (RFC 4514)
Escape a single attribute value for safe interpolation into a DN
component (the part to the right of `=`).

### Active Directory password-recovery helpers

These three helpers are reserved for legitimate password-recovery /
auditing workflows where the operator has Domain Admin and the
"Store password using reversible encryption" group policy is enabled.
A regular LDAP bind cannot read the encrypted blob — retrieving it
requires the DRSUAPI replication protocol (DCSync), which this module
does **not** implement.  Once you have the bytes (typically from
impacket's `secretsdump.py` or equivalent), the helpers below complete
the unwrap step:

#### `rc4(key, data) → string`
Generic RC4 (ARCFOUR) stream cipher.  Cando strings are byte-safe so
keys and outputs may contain arbitrary bytes including NULs.

#### `md5(data) → string` (16 raw bytes)
RFC 1321 MD5.  Returns the 16-byte digest as a binary string.

#### `decode_reversible_password(blob, key) → string`
RC4-decrypts `blob` with `key`, then converts the resulting UTF-16LE
plaintext to UTF-8.  The caller is responsible for deriving `key`
correctly per MS-SAMR §3.1.1.8.10–11; for typical AD setups the key
is `MD5(syskey || rid_le || RPC_CONST_STRING)` — compose with `md5`
above.

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

- The module is synchronous only.  Asynchronous LDAP operations
  (`ldap_search_ext`, `ldap_result`) are not yet exposed.
- SASL mechanisms beyond `simple` (e.g. `GSSAPI`, `EXTERNAL`,
  `DIGEST-MD5`) are not yet exposed; use `bind` with a DN/password.
- `password_modify` with `format="rfc3062"` is POSIX-only;
  `format="ad"` works on both platforms against Active Directory.
- TLS option keys (`tls_cacertfile`, `tls_certfile`, `tls_keyfile`)
  are POSIX-only — Windows reads CA roots from the system trust store.
- `members(..., {recursive:TRUE})` against non-AD directories falls
  back to a client-side BFS; on huge groups this can be many round
  trips.  Use AD or accept the latency.
- `decode_reversible_password` is the unwrap step only; obtaining the
  encrypted blob requires DCSync (DRSUAPI) which is out of scope.
- The connection pool caps at 256 simultaneously open connections.
  `connect()` throws `"connection pool exhausted"` past that.
