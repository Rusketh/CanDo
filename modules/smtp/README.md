# `smtp` module

Email-related functionality.  The module covers SMTP sending, IMAP and
POP3 retrieval, MIME-aware header parsing/building, DKIM signing and
verification, SPF lookups, mailbox-format delivery (mbox / Maildir),
and a handful of DNS helpers used by mail-flow code.

## Loading

```cdo
VAR smtp = include("./modules/smtp/smtp");
```

## SMTP — sending mail

### `smtp.connect(host, port*, opts*) → smtp_conn`

Open an SMTP connection.  Default port is `25`; use `587` for STARTTLS
submission and `465` for implicit-TLS.

`opts`:

| Field         | Type   | Default | Description |
|---------------|--------|---------|-------------|
| `tls`         | string | `"auto"` | `"none"`, `"starttls"`, `"implicit"`, or `"auto"` (decide from port). |
| `verifyPeer`  | bool   | `TRUE`  | Verify the server certificate when TLS is in use. |
| `username`    | string | —       | For AUTH. |
| `password`    | string | —       | |
| `mechanism`   | string | `"auto"` | `"PLAIN"`, `"LOGIN"`, `"CRAM-MD5"`, `"XOAUTH2"`, or `"auto"`. |
| `timeout`     | number | `30000` | ms |

### `smtp.send(opts) → bool`

Convenience: connect, send one message, close.  `opts` accepts the
same fields as `smtp.connect` plus the message envelope (`from`, `to`,
`cc`, `bcc`, `subject`, `text`, `html`, `attachments`).

```cdo
smtp.send({
    host: "smtp.gmail.com",
    port: 587,
    username: "me@example.com",
    password: os.getenv("SMTP_PASSWORD"),
    from: "me@example.com",
    to:   ["alice@example.com"],
    subject: "Hello",
    text:    "Hello from CanDo!",
});
```

### Connection methods

| Method                          | Description |
|---------------------------------|-------------|
| `c:noop()`                      | Round-trip a NOOP. |
| `c:reset()`                     | Reset the transaction (RSET). |
| `c:mailFrom(addr)`              | Begin a transaction. |
| `c:rcptTo(addr)`                | Add a recipient. |
| `c:data(rfc822)`                | Send the raw RFC 5322 message body. |
| `c:close()`                     | QUIT and close. |
| `c:capabilities() → object`     | Server EHLO capability map. |

## Building messages

### `smtp.build(opts) → string`

Produce an RFC 5322 message.  Handles MIME multipart, base64/QP
encoding, and DKIM-friendly canonicalisation.  `opts.attachments` is
an array of `{ filename, contentType, data }`.

```cdo
VAR raw = smtp.build({
    from: "me@example.com",
    to:   ["alice@example.com"],
    subject: "report",
    text: "see attached",
    attachments: [{ filename: "report.pdf",
                    contentType: "application/pdf",
                    data: file.read("report.pdf", "binary") }],
});
```

`smtp.send(opts)` calls `smtp.build` internally; reach for `build`
when you want to sign or store the raw message before sending.

## DKIM and SPF

| Function                                   | Description |
|--------------------------------------------|-------------|
| `smtp.dkimSign(message, opts) → string`    | Add a `DKIM-Signature:` header.  `opts = { domain, selector, privateKey, headers }`. |
| `smtp.dkimVerify(message) → array`         | Verify each `DKIM-Signature` header; returns `[{ ok, signer, error }, ...]`. |
| `smtp.spfCheck(ip, mailFrom, helo) → object` | Resolve the sender's SPF record and return `{ result, explanation }`.  Result is one of `pass`, `fail`, `softfail`, `neutral`, `none`, `temperror`, `permerror`. |

## Header utilities

| Function                                | Description |
|-----------------------------------------|-------------|
| `smtp.parseAddress(s) → object`         | `{ name, address }`. |
| `smtp.parseAddressList(s) → array`      | Comma-split address list. |
| `smtp.formatAddress(name, addr) → string` | Encoded `Name <addr@host>`. |
| `smtp.encodeHeader(s) → string`         | RFC 2047 encoding for non-ASCII. |
| `smtp.decodeHeader(s) → string`         | Inverse. |
| `smtp.parse(rfc822) → object`           | Parse a message into `{ headers, parts, text, html, attachments }`. |

## IMAP

| Function                                       | Description |
|------------------------------------------------|-------------|
| `smtp.imapConnect(host, port, opts*) → conn`   | TLS-by-default IMAPS connection. |
| `c:imapSelect(mailbox)`                        | Select a folder. |
| `c:imapSearch(query) → array`                  | Return UIDs matching a search. |
| `c:imapFetch(uid, parts*) → object`            | Fetch a message. |
| `c:imapMove(uid, mailbox)`                     | Move a message. |
| `c:imapLogout()`                               | Close. |

## POP3

| Function                                  | Description |
|-------------------------------------------|-------------|
| `smtp.popConnect(host, port, opts*) → c`  | POP3 connection. |
| `c:popList() → array`                     | List message IDs and sizes. |
| `c:popRetr(id) → string`                  | Retrieve a message. |
| `c:popDele(id)`                           | Mark for deletion. |
| `c:popQuit()`                             | Apply deletes and close. |

## DNS helpers

| Function                       | Description |
|--------------------------------|-------------|
| `smtp.lookup(name) → array`    | A / AAAA records. |
| `smtp.mx(name) → array`        | MX records, sorted by preference. |
| `smtp.txt(name) → array`       | TXT records. |
| `smtp.ptr(ip) → string`        | Reverse DNS. |

## Local delivery

| Function                                  | Description |
|-------------------------------------------|-------------|
| `smtp.deliverMbox(path, message) → bool`  | Append to a mbox file. |
| `smtp.deliverMaildir(dir, message) → string` | Drop into a Maildir, return the new file path. |

## Capabilities

### `smtp.capabilities() → object`

What the build supports — TLS, DKIM (Ed25519 / RSA), libidn2, etc.
Useful for graceful feature detection.

## Examples

### Send via Gmail

```cdo
VAR smtp = include("./modules/smtp/smtp");

smtp.send({
    host: "smtp.gmail.com",
    port: 587,
    username: "me@example.com",
    password: os.getenv("GMAIL_APP_PASSWORD"),
    from: "me@example.com",
    to:   ["alice@example.com"],
    subject: "ping",
    text:    "hello, world",
});
```

### Verify an inbound message

```cdo
VAR raw = file.read("incoming.eml", "binary");
VAR results = smtp.dkimVerify(raw);
FOR r OF results { print(r.ok, r.signer); }
```

## Errors

SMTP / IMAP / POP errors include the protocol response code and text.
TLS verification failures throw with the OpenSSL error.  Wrap calls in
`TRY` / `CATCH` to handle.
