# SMTP / IMAP / POP3 / MIME Module

A binary extension that gives Cando scripts a complete email surface:
**send, fetch, parse, build, sign, store**.  Loaded the same way the LDAP
module is.

The module is portable C11 backed by:

| Platform | TLS | DNS |
|---|---|---|
| Linux / macOS | OpenSSL | `libresolv` (`res_query`) |
| Windows       | OpenSSL | `dnsapi.lib` (`DnsQuery_A`) |

OpenSSL is the same one cando's `secure_socket` already links against —
no second TLS stack.  No third-party packages are required to build the
module beyond what cando itself already depends on.

## Building

From the repository root:

```bash
make modules                                   # Linux / macOS host
make -C modules/smtp                           # build only this module
make -C modules/smtp test                      # run the C unit tests
```

For Windows, either build natively under MSYS2/MinGW, or cross-compile:

```bash
make -C modules/smtp smtp.dll MINGW_CC=x86_64-w64-mingw32-gcc
```

## Usage

```cando
VAR mail = include("./smtp.so");          // or "./smtp.dll" on Windows
```

Four idioms cover ~90% of email work — every example below stands alone.

### 1. Send (one call)

```cando
mail.send({
    server:   "smtp.gmail.com:587",
    user:     "alice@example.com",
    password: "app-password",
    from:     "Alice <alice@example.com>",
    to:       ["bob@example.com", "carol@example.com"],
    cc:       "team@example.com",
    subject:  "Quarterly report",
    text:     "Hi all,\nSee attached.\n",
    html:     "<p>Hi all,<br>See attached.</p>",
    attachments: [
        { path: "report.pdf" },                       // file on disk
        { name: "data.csv", body: "a,b,c\n1,2,3\n",   // inline string
          contentType: "text/csv" }
    ]
});
```

Returns `{messageId: "<…@example.com>", accepted: [...], rejected: [...]}`.

#### Direct-to-MX (no relay server)

```cando
mail.send({
    from: "noreply@mydomain.com",
    to:   "user@gmail.com",
    subject: "...", text: "...",
    dkim: { selector: "mail", domain: "mydomain.com",
            key: file.read("dkim.key") }
});
```

Module looks up MX records, walks priorities, opens 25, optionally
STARTTLS, delivers.

### 2. Parse and inspect a message

```cando
VAR raw = file.read("sample.eml");
VAR msg = mail.parse(raw);

print(msg.headers.subject);
print(msg.headers.from);
print(msg.text);                          // best text/plain part, decoded
print(msg.html);                          // best text/html part
FOR a OF msg.attachments {
    print(a.filename, a.contentType, a.size);
    file.write("/tmp/" + a.filename, a.body);
}
```

### 3. Fetch (POP3 or IMAP)

```cando
VAR pop = mail.popConnect({host:"pop.example.com", port:995, tls:TRUE,
                           user:"u", password:"p"});
print(mail.popList(pop));                      // [{n:1, size:1234}, …]
VAR raw = mail.popRetr(pop, 1);
mail.popDele(pop, 1);
mail.popQuit(pop);

VAR im = mail.imapConnect({host:"imap.example.com", port:993, tls:TRUE,
                           user:"u", password:"p"});
mail.imapSelect(im, "INBOX");
VAR uids = mail.imapSearch(im, "UNSEEN SINCE 1-Jan-2026");
FOR u OF uids {
    VAR raw = mail.imapFetch(im, u, "RFC822");
    /* parse, process, archive … */
    mail.imapMove(im, u, "Archive");
}
mail.imapLogout(im);
```

### 4. Persistent SMTP session (low-level)

```cando
VAR sess = mail.connect({
    host: "smtp.example.com", port: 587,
    starttls: TRUE,
    auth: { mech: "PLAIN", user: "u", password: "p" }
});

print(mail.capabilities(sess));           // ["STARTTLS","AUTH PLAIN LOGIN", …]

mail.mailFrom(sess, "alice@example.com");
mail.rcptTo  (sess, "bob@example.com");
mail.data    (sess, raw_rfc5322_bytes);   // user-built MIME

mail.reset(sess);                         // RSET — start a new envelope
mail.noop (sess);
mail.close(sess);
```

`sess` is an opaque object holding an internal pool slot index.

## DKIM, SPF, DNS

```cando
/* Sign on send */
mail.send({
    /* … */
    dkim: { selector: "mail", domain: "example.com",
            key: file.read("dkim.key") }
});

/* Sign manually */
VAR signed = mail.dkimSign(raw, {
    selector: "mail", domain: "example.com",
    key: file.read("dkim.key")
});

/* Verify */
VAR result = mail.dkimVerify(raw);
//  → { pass: TRUE, domain: "example.com", selector:"mail", reason:"" }

/* SPF */
print(mail.spfCheck("alice@example.com", "203.0.113.7").result);
//  → "pass" | "fail" | "softfail" | "neutral" | "none" | "temperror" | "permerror"

/* DNS helpers */
mail.mx("example.com");                  // [{priority:10, host:"mx1.example.com"}, …]
mail.txt("example.com");                 // ["v=spf1 …", …]
mail.ptr("203.0.113.7");                 // ["mx.example.com"]
```

## Local delivery: Maildir / mbox

```cando
mail.deliverMaildir(raw, "/var/mail/alice");           /* atomic tmp/→new/ */
mail.deliverMbox   (raw, "/var/mail/alice", "from@x"); /* with `From ` line */
```

## Address parsing / building

```cando
mail.parseAddress("Alice <alice@example.com>");
//  → { name: "Alice", address: "alice@example.com",
//      local: "alice", domain: "example.com" }

mail.parseAddressList("a@b, \"C, D\" <c@d>");
//  → [{name:"", address:"a@b"}, {name:"C, D", address:"c@d"}]

mail.formatAddress({name:"Alice, the Great", address:"a@b"});
//  → "\"Alice, the Great\" <a@b>"

mail.encodeHeader("Schöne Grüße");        /* "=?UTF-8?Q?Sch=C3=B6ne_Gr=C3=BC=C3=9Fe?=" */
mail.decodeHeader("=?UTF-8?Q?Sch=C3=B6ne?=");  /* "Schöne" */
```

## Building MIME without sending

```cando
VAR raw = mail.build({
    from: "x@y", to: "a@b", subject: "Hi",
    text: "plain", html: "<b>html</b>"
});
file.write("out.eml", raw);
```

## Error handling

Every operation that can fail throws a CanDo error.  Catch with
multi-value `CATCH` to inspect the human-readable message, the SMTP
reply code (or 0 for module-internal errors), and the RFC 3463
enhanced status code:

```cando
TRY {
    mail.send({...});
} CATCH (msg, code, enhanced) {
    print(msg);     // formatted module-side message
    print(code);    // 250, 354, 421, 535, 550, 0 for non-SMTP, etc.
    print(enhanced);// "5.1.1" / "" if the server didn't include one
}
```

## Security defaults

The module is opinionated and safe-by-default:

- **TLS on by default** for ports 587 and 465 — explicit
  `starttls: FALSE` is required to send credentials over plaintext.
- **Server-cert verification ON** for client connections (matches
  `secure_socket`).  Set `verifyPeer: FALSE` for self-signed dev
  environments.
- **Header-injection prevention**: any address, subject, or custom
  header value containing bare `\r` or `\n` is rejected with code 0
  before reaching the wire.
- **Connection pool cap** at 256 sessions across all of SMTP / POP3 /
  IMAP combined.  Past that, `connect()` throws
  `"connection pool exhausted"`.

## API reference

### Sending

#### `send(opts) → { messageId, accepted, rejected }`
The high-level workhorse.  `opts`:

| Field | Type | Notes |
|---|---|---|
| `server`   | string | `host` or `host:port`.  Omitted ⇒ direct-to-MX. |
| `user`     | string | Optional AUTH PLAIN username (only when `server` is set). |
| `password` | string | Optional AUTH PLAIN password. |
| `from`     | string | Required.  May include display name. |
| `to`,`cc`,`bcc` | string \| array<string> | At least one required. |
| `replyTo`  | string | |
| `subject`  | string | |
| `text`     | string | |
| `html`     | string | |
| `attachments` | array | `[{path}]` or `[{name, body, contentType?, inline?}]` |
| `headers`  | object | Extra headers to merge in. |
| `dkim`     | object | `{selector, domain, key}` to sign on the way out. |
| `tls`      | bool   | Implicit TLS.  Auto-on for port 465. |
| `starttls` | bool   | STARTTLS upgrade.  Auto-on except port 25. |
| `verifyPeer`| bool  | Default `TRUE`. |
| `timeout`  | number | Seconds; default 30. |
| `raw`      | string | If set, used verbatim instead of building from text/html/attachments. |

### Persistent SMTP session

#### `connect(opts) → Session`
Opens a TCP / TLS connection, runs EHLO, optionally STARTTLS + AUTH.
On success, returns an opaque `Session` handle.

#### `capabilities(sess) → array<string>`
Lines from the post-EHLO `250-` block.

#### `mailFrom(sess, addr) → TRUE`
#### `rcptTo(sess, addr) → TRUE`
#### `data(sess, raw_rfc5322) → TRUE`
The body is automatically dot-stuffed and CRLF-normalised.
#### `reset(sess) → TRUE`
RSET.
#### `noop(sess) → TRUE`
NOOP — useful for keep-alive checks.
#### `close(sess) → TRUE`
QUIT and release the pool slot.

### MIME

#### `parse(raw) → message-object`

Parsed shape:

```cando
{
    headers: {                              /* canonicalised; lower-case keys */
        from:    "Alice <a@x>",             /* string here; structured form is parseAddress */
        to:      "b@y, c@z",
        subject: "Hi",
        date:    "Thu, 8 May 2026 10:00:00 +0000",
        "message-id": "<...@...>",
        /* every other header in lower-case */
    },
    rawHeaders: [["From","Alice <a@x>"], …], /* preserves order, casing */
    text:       "best text/plain, decoded",
    html:       "best text/html, decoded",
    attachments:[{ filename, contentType, contentId, body, size, disposition }]
}
```

Multi-part messages are walked recursively to find the best
`text/plain` and `text/html` leaves.  Binary attachment bodies are
returned as raw byte strings (Cando strings are byte-safe).

#### `build(opts) → raw_bytes`

Same options as `send` (without the network fields).

### Address helpers

#### `parseAddress(s) → { name, address, local, domain }`
Decodes any RFC 2047 encoded-word in the display name.

#### `parseAddressList(s) → [{ name, address }, ...]`
Comma-splits with awareness of quoted strings and angle brackets.

#### `formatAddress({name, address}) → string`
Quotes the display name if it contains commas, `"`, `<`, `>`, or `@`.

#### `encodeHeader(s) → string`
RFC 2047 Q-encoded word (UTF-8).

#### `decodeHeader(s) → string`
Reverses any number of `=?...?=` encoded-words inline.

### DNS

| Function | Returns |
|---|---|
| `mx(domain)` | `[{priority, host}, ...]` sorted ascending |
| `txt(domain)` | `["spf1 ...", ...]` |
| `ptr(ip)` | `["host", ...]` (IPv4 only in v1) |

### SPF / DKIM

#### `spfCheck(sender, ip) → { result }`
`result` is one of `"pass"`, `"fail"`, `"softfail"`, `"neutral"`,
`"none"`, `"temperror"`, `"permerror"`.

Implements: `ip4`, `a`, `mx`, `include`, `all` with all four qualifiers.
Skipped (returns `neutral` for these mechanisms): `ptr`, `exists`,
`exp=`, `redirect=`, macro expansion.

#### `dkimSign(raw, opts) → raw_with_signature`
`opts = { selector, domain, key }` — `key` is a PEM-encoded RSA or
Ed25519 private key.  Adds a `DKIM-Signature: …` header to the front
of the message.

#### `dkimVerify(raw) → { pass, domain, selector, reason }`
Looks up `<selector>._domainkey.<domain>` TXT, RC4-decodes the
public key, and verifies.

### Storage

#### `deliverMaildir(raw, maildir_path) → TRUE`
Atomic `tmp/ → new/` rename per qmail conventions.  Creates the
maildir tree on first delivery.

#### `deliverMbox(raw, mbox_path[, envelope_from]) → TRUE`
Appends to a single-file mbox.  Subsequent lines starting with
`From ` are escaped with a leading `>`.

### POP3

```cando
popConnect(opts) → Session
popList(sess)    → [{n, size}, ...]
popRetr(sess, n) → raw_bytes
popDele(sess, n) → bool
popQuit(sess)    → TRUE
```

### IMAP (narrow surface)

```cando
imapConnect(opts) → Session
imapSelect(sess, mailbox)         → TRUE
imapSearch(sess, query)            → [uid, ...]    /* numbers */
imapFetch (sess, uid, item="RFC822") → raw_bytes
imapMove  (sess, uid, mailbox)     → TRUE
imapLogout(sess)                    → TRUE
```

## Constants

| Constant | Value |
|---|---|
| `VERSION`        | `"1.0.0"` |
| `PORT_SMTP`      | 25 |
| `PORT_SUBMISSION`| 587 |
| `PORT_SMTPS`     | 465 |
| `PORT_POP3`      | 110 |
| `PORT_POP3S`     | 995 |
| `PORT_IMAP`      | 143 |
| `PORT_IMAPS`     | 993 |
| `MAX_LINE`       | 998 |

## Embedding API

The SMTP surface is shipped as a binary module (`smtp.so` / `smtp.dll`)
rather than baked into `libcando`, but it exposes a stable embedder
hook for host applications that want to register the `mail` global
without going through the script-side `include()` machinery:

```c
#include <cando.h>
#include <dlfcn.h>

typedef void (*open_smtplib_fn)(CandoVM *);

CandoVM *vm = cando_open();
cando_open_metalib(vm);
cando_open_socketlib(vm);
cando_open_secure_socketlib(vm);

void *h = dlopen("./smtp.so", RTLD_NOW);
open_smtplib_fn open_smtplib = (open_smtplib_fn)dlsym(h, "cando_open_smtplib");
open_smtplib(vm);                /* registers the global `mail` */
```

Or, equivalently, call the module's `cando_module_init` directly and
push the returned table onto the VM stack (the same path that
`include()` takes internally).

## Limitations / roadmap

The headline-but-not-yet-shipped features are honest about being
deferred:

- **`createServer` (run an MTA)** — design exists but not implemented
  in v1.0.  Receiving mail in v1 is via POP3 / IMAP fetch.  v1.1 will
  add a callback-style server matching the `socket.createServer`
  shape.
- **IMAP `IDLE`** — v1.1.  Fetch is fully synchronous in v1.0.
- **SASL beyond `PLAIN` / `LOGIN` / `XOAUTH2`** — `GSSAPI`,
  `DIGEST-MD5`, `SCRAM-SHA-*` not yet exposed.
- **DKIM** — only `relaxed/relaxed` canonicalisation; only
  `rsa-sha256` and `ed25519-sha256`.  No `l=` body-length tags
  emitted (verify honours them).
- **SPF** — no `ptr` / `exists` / `redirect=` / macro expansion.
  Returns `"neutral"` for unsupported mechanisms rather than
  tempfail.
- **`ptr`/`mx`** — IPv4 only in v1.  IPv6 follows in v1.1.
- **The connection pool** caps at 256 simultaneously open sessions.
  `connect()` / `popConnect()` / `imapConnect()` throw
  `"connection pool exhausted"` past that.

## Testing

The module ships with two layers of tests:

1. **C unit tests** (`test_smtp.c`) — exercise the pure-C parsers
   (base64, quoted-printable, MIME parse/build, address parse,
   dot-stuffing, RFC 2047 decode) without requiring the Cando VM.
   Run with `make -C modules/smtp test`.

2. **Script-level integration tests** (`test_smtp.cdo`) — load the
   compiled module through `include()` and verify the public API
   surface, MIME round-trip, DKIM sign+verify against a local key
   pair, and every error path.  Run with
   `./cando modules/smtp/test_smtp.cdo`.

Operations that need a live mail server (real MX delivery, real
IMAP) are exercised against an intentionally unreachable host so the
error reporter is verified rather than masked.
