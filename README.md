# nghttp2

**HTTP/2 for sysl** — HPACK, and a session that speaks the framing layer. A binding of
[nghttp2](https://nghttp2.org), the library curl, Apache and Envoy use.

*An independent sysl package for nghttp2. It is not affiliated with or endorsed by nghttp2's authors or trademark holders.*

```
dependencies {
  nghttp2 { git = "github.com/sysl-lang/nghttp2", version = "0.3.2" }
}
```

nghttp2 has to be installed. Nothing is vendored — it is a large C project with its own build
system, and it is packaged everywhere:

```
brew install nghttp2
sudo apt install libnghttp2-dev
sudo pacman -S libnghttp2
```

The build needs no flags: nghttp2 installs `libnghttp2.pc` and the manifest asks the machine where
it is.

## The shape

**nghttp2 is never given the socket.** Bytes that arrived go in at `receive`, bytes to write come
out of `send`, and what happened in between is read off as events. HTTP/2 is a *transformation*, and
how bytes reach the wire stays entirely the caller's business.

That is the same arrangement `sh.sysl.openssl` binds TLS in, which is why the two compose: a `h2`
connection is this session's bytes passed through a TLS stream and back, and neither package knows
the other exists. It is also what lets the whole protocol be driven between two sessions in memory,
which is what this package's own tests do — no socket, no port, nothing that can hang.

```sysl
import sh.sysl.nghttp2.{client, header, Event}
import sysl.buf.{Buf, buf}

var h2 = client()?

h2.request([header(":method", "GET"), header(":scheme", "https"),
    header(":path", "/things"), header(":authority", "example.test")])?

loop
    var out: Buf[u8] = buf()

    h2.send(&out)?

    // ... write out.view() to the socket, read what comes back ...

    h2.receive(arrived)?

    loop
        h2.next_event() match
            Some(e) -> e match
                Headers(id, hs, _) -> print("stream", id, "answered")
                Data(id, bytes) -> body.extend(bytes.view())
                StreamClose(id, code) -> return
                _ -> ()
            None -> break
```

**Events rather than callbacks**, and that is the design decision the package turns on. nghttp2
calls back from inside `receive`, which is a place a caller's own code has no business running: it
cannot submit frames there without reentering the library, it cannot fail usefully, and a closure it
registered would have to be kept alive by the session. Recording what happened and handing it over
afterwards costs one copy of each header block and buys ordinary sysl control flow.

## HPACK on its own

The header-compression tier needs no session and is worth having by itself. It is the fiddly,
security-relevant half of HTTP/2 — a decoder that trusts its input is a decompression bomb waiting
for somebody to send it one — and it is identical whoever drives the frames.

```sysl
var enc = deflater()?          // 4096-byte dynamic table, HTTP/2's default
var dec = inflater()?

val block = enc.deflate([header(":status", "200"), header("content-type", "text/plain")])?
val back = dec.inflate(block.view())?
```

**A deflater and an inflater are each a conversation, not a function.** The dynamic table is built
from every block that has gone through, so blocks must be given to one inflater in the order they
arrived. That is HPACK's design: it is what makes the second request on a connection cost five bytes
where the first cost thirty-eight.

`sensitive(name, value)` marks a field that must never be indexed — which is not a comment but a
wire decision. An attacker who can insert requests onto a connection learns a secret header's value
one character at a time by watching whether his guess compressed; keeping it out of the table is the
defence. `authorization` and `cookie` are the fields that want it, and nghttp2 refuses to index
`authorization` on its own even without the flag.

## ALPN

**HTTP/2 over TLS is chosen by ALPN and by nothing else** — no upgrade handshake, no version
header. A client offers a list of protocol names in its ClientHello, the server picks one, and `h2`
is the name that means this.

The list is a wire format rather than a list of strings, which is where this package and a TLS
binding meet: both speak a length-prefixed byte slice and neither has to know the other exists.

```sysl
sv.offer(["h2", "http/1.1"])?              // sh.sysl.openssl, before the handshake

conn.alpn() match                           // ... and after it
    Some("h2") -> ...                       // hand the bytes to a sh.sysl.nghttp2 session
    _ -> ...                                // HTTP/1.1, or the peer offered nothing
```

`offers_h2` and `alpn_list` are here for the ends that have to decide for themselves.

## What is here, and what is not

| | |
|---|---|
| **HPACK** | encode, decode, dynamic table resizing, never-indexed fields |
| **Sessions** | client and server, the connection preface, SETTINGS |
| **Streams** | requests, responses, bodies, trailers, multiplexing |
| **Streamed bodies** | `respond_stream`, `request_stream`, `push` and `finish` — a body written as it is produced |
| **Giving up** | RST_STREAM, GOAWAY, immediate termination |
| **Flow control** | window sizes at connection and stream, WINDOW_UPDATE, and back-pressure with `no_auto_window_update()` and `consume` |
| **ALPN** | `h2` selection over a wire-format list |

## A body written a piece at a time

`request` and `respond` take the bytes they are to send, which is right for a body a program already
has. **A body produced as it goes — an event stream, a proxied response, a file read in pieces — is
the other pair of calls**, and the difference on the wire is *when the head goes out*: a response
whose source never ends still has to say `200` before its first byte of body exists.

```sysl
sv.respond_stream(stream, [header(":status", "200"),
    header("content-type", "text/event-stream")])?

// ... and then, whenever there is something to say:
sv.push(stream, "data: hello\n\n".bytes)?

// ... and at the end, which for an event stream may be never:
sv.finish(stream)?
```

**`push` answers an error where the stream has no open body**, which is the ordinary way a caller
learns the peer gave up: a reset or a close drops the body, so the next push says so rather than
writing into nothing. Trailers may be named at `finish`, which is the one place a streamed body can
carry them — what was unknown until the body ended is exactly what a trailer is for.

`request_stream` is the same on the client side, and it is what an upload of unknown length is.

**Underneath it is nghttp2's deferred/resume path.** A source with nothing to hand over yet answers
`NGHTTP2_ERR_DEFERRED`, which takes the stream out of the write loop until
`nghttp2_session_resume_data` puts it back — where answering zero bytes would be read as an empty
DATA frame and answering EOF would end a stream nobody had finished writing.

**Server push is not bound**, and would not be much use if it were: every major browser has removed
support for it. Extension frames, RFC 9218 priorities and nghttp2's own priority tree are absent for
the ordinary reason that nothing has asked.

**`h2c` — HTTP/2 without TLS — works**, since a session neither knows nor cares where its bytes come
from. What is not bound is the HTTP/1.1 `Upgrade:` dance that converts an existing connection;
prior-knowledge `h2c`, which is what one talks to a service one controls, needs nothing extra.

## Notes for anyone reading the source

**There is no C shim.** All 181 of nghttp2's exported symbols were surveyed and nothing needed
wrapping: the constants are enumerators, which a `c const` block reads, and nothing in the callback
surface crosses by value — so every callback is an ordinary sysl function whose address is taken
with `&f`, rather than an `@export`ed one.

**`nghttp2_frame` is a union, which sysl cannot spell.** Every arm begins with a
`nghttp2_frame_hd`, so the header is readable from the union's own address and `hd.ty` says which
arm is live; the arm is then reached by casting the same pointer, which is what nghttp2's own
examples write as `frame->goaway`. The arms are declared as sysl structs laid over that memory,
which is safe only while the two layouts agree — so `c/tests.sysl` asserts every size and every
offset the layer above reads by name, against `sizeof` and `offsetof` asked of the C compiler.

**Every callback is handed the session's own address as nghttp2's user data**, which is how a C
callback finds its way back to a sysl value. A `&Session` does not move, so that stays true for as
long as the session does — and the session cannot outlive the box, because dropping the box is what
deletes it.

## Testing

```
sysl test .
```

**49 passed, 0 failed.**

The oracle for HPACK is RFC 7541's own Appendix C.4, not nghttp2: three request blocks printed in
hexadecimal with what each decodes to, and none of those numbers came from this library. The three
are one conversation — C.4.2 refers back to a field C.4.1 put in the dynamic table — so decoding
them with a single inflater tests the table rather than the literal encoding. The encoder is checked
the other way: C.4.1's seventeen bytes are pinned byte for byte.

The session tests wire a client and a server to each other in memory and assert on the whole
transcript of events, which is how a real preface, real flow control and real interleaving get
checked with nothing that can hang.

Run under AddressSanitizer with `SYSL_EXTRA_CFLAGS="-fsanitize=address -g" sysl test .` — clean, and
`nm -u` confirms 20 ASan symbols including `__asan_memcpy`, so the instrumentation is real. **It
covers the sysl half only**: nghttp2 is resolved through `pkg_config`, so its objects were compiled
by whoever packaged it and no flag here reaches them.

**Checked for leaks by measurement, because no test can see one.** 50,000 rounds of a client, a
server, a request, a response, a deflater and an inflater — 200,000 nghttp2 handles — hold the
resident set at 2.5 MB against 2.3 MB for 5,000 rounds. Every constructor here answers a `&T`, which
is what makes the destructors run at all: a `Drop` fires when a box's strong count reaches zero and
for nothing else, so a constructor returning a bare value would leak one handle per call and
compress, parse and serve perfectly the whole time.
