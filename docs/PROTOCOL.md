# CacheX Wire Protocol v1

A line-based text protocol over TCP. Deliberately not RESP — the goal here is a
protocol you can drive with `netcat` and read with your eyes.

> **Status:** ✅ implemented in Stage 5. This document is the specification; the
> parser in `src/net/protocol.cpp` is the implementation, and
> `tests/protocol_test.cpp` is the executable version of this page.

---

## 1. Transport

- **TCP**, default port **6379**, default bind address **127.0.0.1**.
- The connection carries a byte stream in both directions. **There are no message boundaries at the transport layer** — the protocol below supplies them.
- `TCP_NODELAY` is set on both ends. Without it, Nagle's algorithm plus delayed ACKs add up to ~40 ms per round trip on a request/response workload.

## 2. Framing

**One request per line. One reply per line. The terminator is `\n` (LF).**

- A trailing `\r` before the `\n` is accepted and stripped, so `CRLF` clients (telnet, most hand-rolled ones) work unchanged. Replies always use bare `\n`.
- The framing rule is the entire reason the client and server can find message boundaries in a stream that has none. See ARCHITECTURE §7.4.

### Limits

| Limit | Value | What happens if exceeded |
| --- | --- | --- |
| Request line | **65 536 bytes** including terminator | Server replies `-ERR line too long...` and **closes the connection** |
| TTL | `0` … `315360000` seconds (10 years) | `-ERR TTL too large` |

The line limit exists because a client that never sends `\n` would otherwise make
the server buffer until it ran out of memory. The connection is closed rather
than resynchronised: recovering would mean discarding input until the next
newline, and a line this long is a protocol violation rather than a typo. Redis
closes the connection in the same situation.

## 3. Requests

Tokens are separated by spaces or tabs; runs of separators collapse, so
`GET   foo` is the same as `GET foo`. Command names are **case-insensitive**.

| Command | Arguments | Reply |
| --- | --- | --- |
| `SET key value` | 2 | `+OK` |
| `SET key value ttl_seconds` | 3 | `+OK` |
| `GET key` | 1 | `=value` or `_` |
| `DELETE key` (alias `DEL`) | 1 | `:1` if removed, `:0` if absent |
| `EXISTS key` | 1 | `:1` or `:0` |
| `TTL key` | 1 | `:seconds`, `+NOEXPIRE`, or `_` |
| `PING` | 0 | `+PONG` |
| `QUIT` | 0 | `+BYE`, then the server closes the connection |

### ⚠️ Keys and values cannot contain whitespace or newlines

Because tokens are whitespace-delimited, a key or value containing a space,
tab, `\r` or `\n` cannot be expressed. The cache itself stores arbitrary bytes
(including embedded NULs) — this is a limitation of *this protocol*, not of the
engine underneath it.

**This is the clearest argument for length-prefixed framing.** RESP sends
`$3\r\nfoo\r\n`: the length comes first, so the payload needs no escaping and
can hold any byte at all. A text protocol has to choose between delimiters and
arbitrary content. Adopting length-prefixed framing is future work.

## 4. Replies

Every reply is one line, and **the first byte is a type tag** — so a client can
dispatch on a single character without parsing the rest.

| Tag | Form | Meaning |
| --- | --- | --- |
| `+` | `+OK`, `+PONG`, `+BYE`, `+NOEXPIRE` | Simple status |
| `=` | `=<value>` | A value; everything after `=` up to the newline |
| `_` | `_` | Nil — the key does not exist (or has expired) |
| `:` | `:<integer>` | An integer |
| `-` | `-ERR <message>` | Error; the connection stays open unless stated otherwise |

### Why `_` and `+NOEXPIRE` instead of magic numbers

Redis's `TTL` returns `-2` for a missing key and `-1` for a key with no expiry,
overloading one integer with two sentinel values. CacheX gives each case its own
reply type, so `:` always means "this really is a number" and no client can
accidentally do arithmetic on "missing". It is the same reasoning behind
`TtlInfo` being a tagged type rather than an `int` (ARCHITECTURE §6.1).

## 5. Errors

Errors are replies, not disconnections. A client that sends nonsense gets told
what was wrong and may carry on using the connection.

| Condition | Reply |
| --- | --- |
| Unrecognised verb | `-ERR unknown command 'FOO'` |
| Wrong argument count | `-ERR wrong number of arguments for 'GET' (expected 'GET key')` |
| Non-numeric TTL | `-ERR invalid TTL 'abc' (expected a whole number of seconds)` |
| Negative TTL | `-ERR TTL must not be negative` |
| TTL above the maximum | `-ERR TTL too large (maximum 315360000 seconds)` |
| Blank line | `-ERR empty command` |
| Line over the limit | `-ERR line too long (...); closing connection` — **connection closes** |

Echoed tokens are truncated to 32 characters, so a 60 KB junk token cannot be
reflected back in full.

**The only error that closes the connection is the line-length violation**,
because it is a framing failure: the server no longer knows where the next
command begins.

## 6. TTL semantics over the wire

The protocol speaks **seconds**; the cache tracks milliseconds internally.

- `TTL` **rounds up**: a key with 900 ms left reports `:1`, not `:0`. Reporting `:0` for a key that is still readable would be misleading.
- `SET key value 0` **deletes the key** — consistent with the cache's rule that after `set(k, v, ttl)` the key is visible iff `ttl > 0`. (Redis differs: it rejects `SET ... EX 0` as an error.)
- `SET key value` with no TTL on a key that *had* one **clears the TTL**. A `SET` replaces the whole entry, expiry included.

## 7. Connection lifecycle

```
  client                                    server
    |                                          |
    |                          socket/bind/listen  (once, at startup)
    |                                          |
    |------------- connect() ----------------->|  3-way handshake, kernel queues
    |                                          |  the finished connection
    |                                    accept() returns a NEW socket
    |                                          |
    |--- "SET foo bar\n" --------------------->|  recv -> buffer -> frame -> parse
    |<-- "+OK\n" ------------------------------|  -> execute -> format -> send
    |                                          |
    |--- "GET foo\n" ------------------------->|      (repeat any number of times
    |<-- "=bar\n" -----------------------------|       on the same connection)
    |                                          |
    |--- "QUIT\n" ---------------------------->|
    |<-- "+BYE\n" -----------------------------|
    |<------------ close() --------------------|
```

A connection may carry **any number of commands**. Opening a connection per
request would pay a full handshake each time.

Three ways a connection ends:

1. **`QUIT`** — the server replies `+BYE` *then* closes, so the client sees the acknowledgement before the socket goes away.
2. **Client disconnects** — `recv()` returns `0`, which is end-of-stream, not an error and not "no data yet". The server closes its side and goes back to `accept()`.
3. **Framing violation** — the over-long-line case above.

If the client vanishes mid-reply, `send()` fails; the server abandons the
connection and carries on. It does not die of `SIGPIPE`, which is suppressed via
`MSG_NOSIGNAL` (Linux) or `SO_NOSIGPIPE` (macOS/BSD).

## 8. Concurrency

**The Stage 5 server serves exactly one client at a time.** A second client
completes its TCP handshake — the kernel does that independently — and then waits
in the accept backlog (128 deep) until the first client disconnects.

This is a real limitation, not an oversight: the cache is not thread-safe, so
serving two clients in parallel today would be a data race. Concurrency is
Stage 8.

## 9. Worked example

```console
$ ./cachex_server 6379 &
$ nc 127.0.0.1 6379
PING
+PONG
SET foo bar
+OK
GET foo
=bar
EXISTS foo
:1
TTL foo
+NOEXPIRE
SET sess abc 60
+OK
TTL sess
:60
DELETE foo
:1
GET foo
_
BOGUS x
-ERR unknown command 'BOGUS'
GET
-ERR wrong number of arguments for 'GET' (expected 'GET key')
QUIT
+BYE
```

## 10. Not in v1

- Length-prefixed framing (so keys/values could hold arbitrary bytes)
- Pipelining guarantees — the server *does* handle multiple buffered commands per read, but the protocol makes no promise about it
- `EXPIRE` / `PERSIST` (changing a TTL without rewriting the value)
- `KEYS`, `SCAN`, `FLUSH`, `INFO`, `DBSIZE`
- Authentication and TLS — CacheX assumes a trusted local network
- Any binary encoding
