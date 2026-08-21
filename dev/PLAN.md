# Plan

Open steps, in order. A step is closed here when its evidence is in the repository:
a test that fails without it, a measurement, or both. Issue numbers point at
`true-async/server` unless another repository is named.

## In progress

- [x] **#170 — a streamed response is flushed per chunk.** `flush` op on the encoder
  vtable (`Z_SYNC_FLUSH`, `ZSTD_e_flush`, `BROTLI_OPERATION_FLUSH`), called from
  `ws_append_chunk` for every non-empty chunk. Unconditional on the streaming path,
  no config key. Evidence: `013-h1-streaming-gzip-flush.phpt` decodes the first chunk
  while the handler is still parked and 0 bytes without the fix; `042` and `051` show
  Brotli and zstd going from nothing on the wire to a flushed block; `CompressionGzip`
  and the new `CompressionFlush` unit tests decode the flushed prefix with each
  codec's own decoder, including through the `NEED_OUTPUT` loop; cost measured in
  `dev/BENCHMARKS.md`. Merged.

## Next

- [ ] **Drop the streaming exemption in laravel-spawn.** `TrueAsyncServer::streamContent`
  calls `setNoCompression()` on every `StreamedResponse` as the workaround for #170
  (YanGusik/laravel-spawn#57). Remove it once a build with the flush is in use, and
  re-measure the CSV timeline from the issue.
- [x] **#171 — a streaming response cannot be aborted.** Merged as PR 193 (`cf1c708`). A body truncated by an
  exception read as complete on the wire. `abort` is an op on
  `http_response_stream_ops_t` answering `bool`, and the answer carries the
  distinction the design first missed: false means the transport has not put a
  byte of this response on the wire, so there is nothing to fail and the empty
  response each transport commits lazily is the better answer —
  `sseStart()` followed by a throw keeps its 200. HTTP/1 withholds the
  terminator and drops keep-alive, HTTP/2 sends RST_STREAM(INTERNAL_ERROR),
  HTTP/3 resets the write side with H3_INTERNAL_ERROR, the pool worker posts
  `RESPONSE_WIRE_STREAM_ABORT`, which already existed and was reachable only
  from the two places a wire is lost — an undeliverable post and a credit wait
  that timed out — never from the API. Dispose routes to it on an uncaught exception but **not** on a
  cancellation: the server cancels its own handlers at graceful shutdown, and
  an open feed cut short there has produced every byte it promised.

  Evidence: `h1/033` reads the wire and finds the frames but no terminator,
  `h1/034` keeps the empty-200 shape, `h1/035` proves the pipelined request
  behind a failed stream is not answered, `core/063` walks the state machine,
  `h2/053` reads error code 2 off the RST frame and completes a second request
  on the same connection, `h3/058` and `h3/059` get `RESET(err=258)` out of
  aioquic, `h2/053` also reads back a code the handler named, `grpc/018` keeps
  `grpc-status: 13`, `compression/072` makes the
  decoder refuse the bytes, `h1/036` holds the carve-out — a handler parked in
  a delay and cancelled by `stop()` still gets its terminator, and loses it
  when the carve-out is removed — and `compression/073` runs the whole
  dispose-through-the-wrapper route against a peer that has gone.

  Six defects the work uncovered, all fixed here. **A compressed stream whose
  handler forgot `end()` was undecodable**: dispose finished at transport level,
  below the wrapper that owns the codec trailer, so `gzdecode()` refused a body
  the framing called complete — every dispose path now finishes through
  `response->stream_ops` (`compression/071`). **The pool's abort could ship a
  clean FIN**: the reactor applies a response's wires in one tick and flushes at
  the end of it, so raising the end flag before that flush let the data reader
  emit EOF first; observed once in three runs. **A pipelined request behind a
  failed HTTP/1 stream was still answered**, which put a whole `HTTP/1.1 200 OK`
  where the peer was counting down an unfinished chunk. **A locally sent
  RST_STREAM came back as a peer reset**, cancelling the handler that asked for
  it with "stream reset by peer" and inflating the peer-reset metric. **HTTP/2
  published its chunk ring before the headers it belongs to**, so a failed
  header commit left a ring behind and abort read it as "the peer has heard
  from us". **The pool marked a stream started before its headers wire was
  away**, so an abort was posted against a stream the reactor never opened.

  Two corrections came from review and are carried without a test, which is
  worth naming. The cancel carve-out keys on the exception's class rather than
  on the cancelled flag, so that the parse-error and peer-reset cancels — both
  real truncations — cannot be mistaken for a graceful shutdown; on today's
  paths the peer-closed latches already stop the terminator, and no shape
  reached from PHP tells the two versions apart. And the HTTP/2 wait for ring
  room refuses to park on a peer that has gone, which matters because dispose
  now reaches that wait through the compressing wrapper and a coroutine parked
  there is past cancellation; the ring drains as a handler fills it, so the
  reproduction needs a peer that is simultaneously alive enough to hold the
  window shut and gone enough to be latched, and I could not build one.

## The response body contract (YanGusik/laravel-spawn#50, #60)

Two field reports from one user came from the API rather than from his code, and
the second arrived 44 minutes after the first was closed. `write()` buffers until
`end()`, while Node, Swoole and Go all send under that name; `sendable()` folds
four questions into one bool and `README.md:277` documents it as a liveness check,
which is the loop that truncated his stream, and which laravel-spawn had already
copied into `Sse::connected()`; a handler-declared `Content-Length` reaches the
wire unverified. The contract is settled; the steps are ordered so the
documentation lands first, because the reporter is writing a proxy recipe against
it and expects a tag within days.

- [x] **Document the three body modes first.** `docs/USAGE.md` says nothing about
  the response body at all. It gains a section naming the modes — buffered
  (`setBody`), streamed (`write`), file (`sendFile`) — the state each commits, and
  a framing table: a buffered body gets its `Content-Length` computed, an
  undeclared stream is chunked or DATA frames, a declared stream keeps the header.
  `README.md:277` and the `write()` docblock (`stubs/HttpResponse.php:160`, which
  never says that nothing leaves before `end()`) are corrected in the same step.
  Done in #174: the README guard is gone and both docblocks say what they mean.
  The `docs/USAGE.md` section landed with the renames as §3.5, written once
  against the final names.
- [x] **`isWritable(): bool` — liveness, with the op behind it.** A new optional
  `is_alive` in `http_response_stream_ops_t` (`include/php_http_server.h:706`);
  every backend already computes it inside `append_chunk` (`peer_closed` for H2,
  `stream_credit_is_dead` for the worker). Sound as a predicate because every
  input is a one-way latch, unlike queue depth.
- [x] **`write()` becomes the streaming call.** Done in #180. `send()` is removed
  outright rather than kept as a deprecated alias: it would have covered one call
  in the shipped laravel-spawn adapter while `isClosed()` breaks three others
  beside it (`src/Server/TrueAsyncServer.php:106,395,413,492`), so the adapter
  needs a release either way. Buffered incremental appending keeps its behaviour
  under `appendBody()`; `isClosed()` became `isEnded()`;
  `sendable()` throws a tombstone naming `isWritable()` and `tryWrite()`;
  `setBodyStream()`/`getBodyStream()` are deleted. Evidence:
  `tests/phpt/server/core/062-body-api-names.phpt` reads the wire for each mode,
  and `h2/023-h2-sendable-tombstone.phpt` asserts the throw on a live H2 stream
  where the method used to answer. `docs/USAGE.md` §3.5 documents the three modes.

  Two defects surfaced while doing it, both fixed here. **#181**: a buffered body
  followed by a streaming call was discarded with no error — the streaming path
  never reads `response->body`, while the reverse direction has always thrown, so
  only one side of a symmetric-looking mistake reported. **The `stream` perf
  profile had never run**: `tests/perf/servers/server_stream.php` called `->send()`
  with no argument against an arginfo requiring one, so the profile answered 500
  with `expects exactly 1 argument, 0 given` before measuring anything, and its
  chunk loop buffered through the old `write()`.
- [~] **`tryWrite(): bool` and the dialect twins.** In #178, without the twins. The non-blocking half of the
  pair, matching `WebSocket::trySend()`; `trySseEvent()` and `tryWriteMessage()`
  follow, so the idiom is not half-applied. Invariant: false means nothing was
  queued and no header was committed, and a dead peer is still the 499 exception.
  Blocked by the compressing wrapper — `ws_append_chunk` feeds the encoder and
  closes a block before it consults the underlying ops, so a refusal there is not
  retryable, and the capacity check has to move ahead of the encoder.
  Three review passes reshaped it. The refusal moved into `append_chunk` as a
  `nonblocking` argument, because a predicate read beforehand cannot be atomic at
  the PHP boundary; the wait moved into a `wait_writable` op, because each
  transport's own wait carries a deadline, a wake source and a re-pump of the
  drain that a wait assembled outside would drop; `awaitWritable()` answers false
  rather than true where a transport can be full but offers no wait, since "go
  ahead" spins a handler that trusts it. `compressing_stream_ops` and
  `h3_stream_ops` gained the missing `sendable` slots — without them a refusal
  under compression threw away a block the encoder had already emitted, and the
  retry the caller was told to make corrupted the deflate stream.

  **HTTP/1 is the open exception**: it keeps no queue of its own, so it never
  refuses and an accepted chunk waits for the socket. Two ways to close it were
  tried and rejected — see below, and the measurement that closed the argument is
  in `dev/BENCHMARKS.md`.
- [x] **The dialect twins: `trySseEvent()` and `tryWriteMessage()`.** The idiom is
  half-applied while `tryWrite()` has a non-blocking form and the two dialects do
  not. Shape: one static helper per dialect carries the formatting and the guards,
  and the blocking and non-blocking entry points differ only in the flag they pass
  to `append_chunk` — `sseEvent()` and `writeMessage()` move onto it rather than
  keeping a second copy. Both new methods answer `bool` on the `tryWrite()`
  contract: false means nothing was queued and no header was committed, a dead peer
  is still the 499 exception, and HTTP/1 never refuses because it has no queue to
  refuse from. Evidence to produce: phpt on H1 and H2 that a refusal leaves the wire
  untouched, that the bytes match the blocking twin, and that the peer's death still
  throws; `docs/USAGE.md` §3.5 and a CHANGELOG entry.

  Done. Evidence: `h1/031` shows HTTP/1 accepting every offer and the record matching
  `sseEvent()` byte for byte; `h2/026` and `h2/027` fill the ring, take a refusal, wait it
  out and end with a body identical to the blocking twin's — `027` alternates the two
  twins, so the same body also proves they frame alike; `h1/032` aborts the peer and gets
  499 out of both, on a response that is still uncommitted.

  Four defects surfaced and are fixed here. `awaitWritable()` was refused in SSE mode by
  the guard that keeps `write()` out of it, so the handler the twins were written for had
  nowhere to wait — waiting emits nothing, so that guard and the buffered-body one now
  apply only to the calls that emit. `writeMessage()` never answered to those guards at
  all: a gRPC frame could be pushed into an SSE body or discard a buffered one, and the
  refactor would have copied the hole onto a second public method. `trySseEvent()` started
  the stream before probing the peer, so a 499 left the response committed and the handler
  could not answer with a status. And `awaitWritable($timeoutMs)` documented a deadline no
  transport read: HTTP/2 now waits the shorter of it and the connection's write timeout.
- [x] **Framing by declared length.** Merged as PR 195 (`d44e749`), two commits:
  the step, and the answer to its review. A `Content-Length` set before the first
  `write()` reaches the client verbatim on every protocol, and the server becomes
  the auditor: excess throws at the offending write, a shortfall aborts the stream
  at `end()` instead of finishing cleanly under a header that lies. Compression is
  disabled for such a response. `src/http1/http1_format.c:249` is the only path
  that passes a handler value today; H2, H3 and the worker strip it in
  `http_response_header_allowed_h2h3`. The abort op it waited on landed with #171.

  The declaration is taken at the first `write()`, into `declared_length` on the
  response, and the count is **reserved** before each chunk reaches the transport
  rather than added after it returns: every `append_chunk` suspends, so a second
  coroutine writing to the same response would otherwise be admitted against a
  total already spoken for. The shortfall verdict sits in
  `http_response_finish_stream`, the one finisher `end()`, `abort()` and all four
  dispose paths pass through, and it compares for equality — an overshoot the
  reservation cannot produce today would be failed too rather than trusted.

  Four carve-outs decide what declares. **gRPC** closes its body with a trailer
  frame the handler never sees, and `grpc_call_finish` is a fifth finisher that
  bypasses `finish_stream` — a declared length there would be carried and never
  audited, so `grpc_mode != 0` declares nothing. **SSE** starts its stream
  through `sse_ensure_started` and keeps its framing. **HEAD** returns before
  the guard, so on HTTP/1 the length stays on the buffered path where it
  describes the body a `GET` would return; on HTTP/2 and HTTP/3 it is stripped
  there as it always was, which is the open step below. And a status defined to
  carry no content (`1xx`, `204`, `304`) declares nothing, so such a response
  keeps the framing it had — chunked, which RFC 9112 §6.1 forbids on it just as
  it forbids the identity body: the carve-out declines to widen that defect
  rather than closing it, and it belongs to the HTTP/1.0 step below.

  A declaration is not always what reaches the client. When the body ends short
  and `abort()` answers false — nothing on the wire — the finisher rewrites the
  header to the count actually written, because the empty response committed
  behind it would otherwise leave the peer counting down to bytes that are not
  coming.

  What a declared stream must not do is let bytes past the guard. `write()`,
  `tryWrite()`, `end($data)` and both message calls go through it;
  `writeMessage()` needs no gRPC mode of its own, so on a plain request it would
  otherwise have put an uncounted frame onto an identity-framed body — found by
  the intent reviewer on the finished diff, fixed here, pinned by `core/064`
  route `/frame`. A declaration is therefore taken by whichever streaming call
  comes first, and the byte counted is the one that call puts on the wire, not
  the payload it was handed.

  The reservation is given back whenever the transport queued nothing — a
  non-blocking refusal, and every `HTTP_STREAM_APPEND_STREAM_DEAD`. Keeping it
  on a dead stream let a body that never reached the peer satisfy the count, so
  HTTP/2 ended cleanly under a `content-length` that overstated it. And the
  declaration is adopted only once a chunk is accepted: a first offer refused
  for over-run leaves the response uncommitted and free to answer buffered, and
  a length recorded before that check reached the HTTP/3 field section beside a
  body of another size. Both found by the adversarial reviewer on the finished
  diff.

  Two limits are known and carried without a test. The header correction on the
  abort-answers-false branch needs a transport that refuses its first chunk
  before opening the stream, which no shape reachable from PHP produces today.
  And on the pool path the worker counts a chunk once the wire is posted, while
  the reactor may drop it for a stream that has moved on (`http3_dispatch.c`
  `RESPONSE_WIRE_STREAM_CHUNK`) — the count is a floor there, so a declared
  length can be reported kept when the reactor discarded part of it.

  Evidence: `h1/037` reads nine body bytes with no framing around them and
  completes a second request on the same connection, `h1/038` refuses the
  offending chunk and still finishes the promised body, `h1/039` fails a short
  one, `core/064` walks the declaration's edges from PHP — malformed, twice,
  late, exact — and proves the malformed case leaves the response uncommitted
  and answerable with a 500, `h2/054` and `h3/060` read the field off the wire
  against an undeclared control and get a reset on a short body, `h3/061`
  carries it across the pool wire, `compression/074` shows the same payload
  gzipped without a declaration and identity with one.

  Two defects the work uncovered. **A handler `Content-Length` that disagreed
  with a buffered body desynced the connection**: the value was emitted verbatim
  next to whatever body the response held, so eleven bytes under a header naming
  five left the client reading the last six as the next status line. The server
  now states the count it is sending, except on `HEAD`, and `core/064` route
  `/buffered` pins it. And **one predicate dropped both framing headers**, so keeping `Content-Length` on the streaming
  path would have kept a handler's `Transfer-Encoding` with it — the pair RFC
  9112 §6.1 forbids and §6.3 names as the smuggling shape; the two names are
  dropped separately now.

  Reviewed by four critics against the design before any code: they found the
  gRPC finisher bypass, the commit-then-throw ordering, the
  two-headers-one-predicate lever, the HEAD claim that held for `write()` only,
  and the check-then-count race. Reviewed again on the finished diff, which is
  where the `writeMessage()` hole and the missing test for the buffered fix came
  from. Each finding is fixed above or an open step below.

- [x] **#197 — an HTTP/1 message carries the body its status and method allow,
  framed the way the request can read.** Merged as PR 199 (`6892b97`), eleven
  commits: three for the step, six answering two rounds of critic review, one
  for a pass over every comment it adds. The step opened as one defect and the
  critics turned it into six, all of the same shape: the response frames itself
  from the declaration alone and reads neither the request version, nor the
  method, nor its own status.

  One predicate now answers for all of it — `h1_response_framing` in
  `src/http1/http1_format.c`, four values from RFC 9112 §6.3's eight receiver
  rules: none (rule 1), length (5), chunked (3), close-delimited (7). The
  streaming formatter and the stream vtable read it. The buffered formatter
  states a length of its own, so it answers the same two questions — does this
  status carry a body, does this method — from the same inputs rather than from
  the enum; what is gone is the third place that used to decide, not the
  second.

  **HTTP/1.0.** `Transfer-Encoding: chunked` went to every 1.0 client, which has
  no decoder for it (§6.1). Such a body is delimited by the close now, with
  `Connection: close` in the header block. That costs the connection, so a
  pipelined request behind it goes unanswered: `ctx->close_delimited` joins
  `stream_dead` in the `framing_lost` gate, without which the drain answers the
  next request *into* the body and `http_connection_on_request_ready` re-raises
  `conn->keep_alive` from it, so the EOF that ends the first response never
  arrives. Two critics found that independently.

  **The connection was never told how it ends.** A streaming response committed
  its header block at the first `write()` and the `Connection` decision was
  taken in dispose, after those bytes had left — so a request that asked for
  close and a graceful drain both went unmentioned to every streaming client.
  The decision moved into `h1_streaming_headers_build`, which is the only place
  a streaming response can state it. Dispose still asks, because a stream
  outlives the answer — see the correction below. The 1.0 keep-alive echo
  (RFC 2068 §19.7.1) lands in the same place and in dispose for the buffered
  path — without it "a 1.0 client that declares a length keeps keep-alive" was
  server-side fiction.

  **A status that carries no body.** Deferred here by the declared-length step.
  A streaming call throws while the response is uncommitted; a buffered body is
  dropped at format time, because the status may legitimately be chosen after
  the body was built and there is no one left to tell. `sseStart()` refuses;
  a 304 keeps a handler `Content-Length` (RFC 9110 §8.6) and a 1xx and a 204
  lose it (§8.6). The refusal is in the shared `write()`, so it reaches HTTP/2
  and HTTP/3 too.

  **`HEAD` shipped a body** through `sseEvent()` and `writeMessage()`, which
  never had the drop `write()` and `tryWrite()` do.

  **The buffered path sent `Content-Length` beside a handler
  `Transfer-Encoding`** — the pair §6.1 forbids and §6.3 names as the smuggling
  shape. The streaming path stopped in #195; the buffered one drops it now.

  **The buffered drop was HTTP/1's alone, while the docs and the test said it
  was everyone's.** Found by three of the four critics on the finished diff and
  confirmed on the wire with this repository's own HTTP/2 client: a 204 with a
  buffered body put `DATA("oops")` on the wire, a 304 did the same, and a
  `HEAD` shipped the whole body — the last one on `main` too, since HTTP/2 had
  no `HEAD` suppression at all. The drop now sits where each transport reads
  the body for the wire, in `http_response_get_body` and
  `http_response_get_body_str`, so HTTP/2, HTTP/3 and the pool worker inherit
  it from one place and HTTP/3's own `is_head` branch is gone. `h2/055` had
  been reading the body with curl, which discards content on a 204 whatever the
  server sent; it counts frames now and asserts no stream was reset, so an
  empty body proves the server dropped it rather than the client hiding it.

  **Five defects the review found and this step fixes.** Recorded together
  because they are one family — the server states a message's framing, and each
  of these was a place where it did not.

  **A header value reached the wire unfiltered**, which is #198's other half
  and the reachable one: `setHeader('Location', $userInput)` with a CRLF ended
  the header block and put a second response behind it (CWE-113). `setHeader()`,
  `addHeader()` and `redirect()` refuse a name that is not an RFC 9110 §5.6.2
  token and a value carrying a byte §5.5 does not allow. Refused rather than
  cleaned, because these run while the handler can still answer; `redirect()`
  checks before assigning the status, so a refused call changes nothing.
  Evidence: `h1/046`.

  **A handler `Connection: close` was copied and ignored**: the field reached
  the peer while `conn->keep_alive` stayed true, so the next request was
  answered on a socket the client had retired, and on 1.0 the keep-alive echo
  overwrote the handler's value instead. The field is taken as the request it
  is now — the connection closes, and the peer is told by the branch that
  already tells it about a drain. **`Transfer-Encoding` was dropped whatever it
  named**, so a handler that set `gzip` sent encoded bytes with no coding
  declared; only `chunked` is accepted and dropped now. Evidence: `h1/047`,
  which writes a second request onto the same socket — the header alone never
  proved anything.

  **A 1xx framed correctly and left the exchange unfinished.** It is an interim
  response (RFC 9110 §15.2) and nothing follows it, so both ends waited for the
  other; to a 1.0 client, which §15.2 forbids it to entirely, it read as the
  answer. `setStatusCode()` takes 200 to 599 now. `core/027` pinned the old
  boundary and changes with it — the one contract in this step that was already
  written down, and worth naming for that reason.

  **205 carried a body**, which RFC 9110 §15.3.6 forbids, and could not have
  framed one: §6.3 rule 1 does not name 205, so a client still looks for
  framing. Its own leg — body dropped, `Content-Length: 0` stated — rather than
  joining 204 and 304, which state no length at all.

  **A `HEAD` whose handler streamed reported `Content-Length: 0`**, because
  `write()` returned before the stream committed and the buffered path computed
  a length from an empty buffer. The stream commits now and only the bytes are
  dropped, which also makes the four streaming calls agree with the two SSE
  ones.

  **A second review round, and eight more findings.** The six commits above
  went back to four critics, and what they returned is recorded here because
  one of it was a regression this step had just introduced.

  **Committing a HEAD stream threw the handler's failure away.** The first
  shape of the HEAD fix called `http_response_stream_commit_once`, which sets
  `committed`, and the dispose path derives a 500 from an uncaught exception
  only while the response is uncommitted. A `HEAD` handler that wrote a chunk
  and then threw answered `200 OK` with a clean header block — and on a message
  that ends at the header block a failed response is byte-identical to a
  successful one, so the peer cannot tell. Health checks read it as green.
  Confirmed on the wire before the fix. The commit is gone: a dropped chunk
  records `head_streamed` and nothing else, so the response stays uncommitted,
  `setHeader()` goes on working, and the buffered formatter reads the flag
  instead of measuring an empty buffer. That also closed three findings the
  same commit had opened — `end($data)` reaching `append_chunk` on a HEAD over
  HTTP/2, a HEAD losing the ability to state a length it measured, and the pool
  worker reverting such a response to a buffered wire.

  **`sendFile()` and `json()` wrote `status_code` past `setStatusCode()`.**
  `new SendFileOptions(status: 204)` framed a whole file under a status the
  client ends at the blank line; `json($d, 100)` produced the interim response
  that hangs the exchange. One predicate answers for all three now, and
  `sendFile()` also refuses a status defined to carry no content.

  **An object value walked past the header check.** It tested the zval's type
  and skipped anything that was not a string, while storage converts — so a
  PSR-7 URI built from a query parameter, which is the commonest way request
  data reaches `Location`, went through unchecked. The bytes checked are the
  bytes stored now. Leading and trailing whitespace is refused in the same
  place (§5.5), and `setTrailer()` answers to the check as well: HTTP/1 emits
  no trailers today, but gRPC puts an exception message into `grpc-message`,
  and the day a chunked-trailer emitter lands there that is CWE-113 with
  nothing in its way.

  **`Connection: keep-alive` was refused where a drop was right.** The shape
  that sets it is a handler copying an upstream response's headers wholesale,
  and refusing it turns a correct response into a 500 over a field the server
  ignores. It is dropped; `close` is still read; anything else still throws.
  `resetHeaders()` clears the recorded close, which it did not, and the flag is
  documented as HTTP/1-only, which the first shape of it was not.

  **Compression did not know about 205**, so a 205 with a compressible body got
  `Content-Encoding: gzip` beside `Content-Length: 0` and no bytes. It reads
  `response_status_carries_body` now, so the list cannot drift again.

  **A drain that came due mid-stream was lost.** The first shape of this step
  gated dispose's whole `Connection` block on `http_response_is_streaming`, to
  keep the drain evaluator from being asked twice for one response. The
  evaluator is idempotent once latched — both its arms are guarded, and the
  second call returns the same verdict and increments nothing — so what the
  gate bought was nothing, and what it cost was every drain that arrives after
  the header block: a connection that reached its age limit while its body was
  going out kept `keep_alive` and was handed on for reuse, which `main` did not
  do. Dispose asks again now. What a streaming response does not do there is
  touch the header block — those bytes may already have left — so the telling
  and the counter stay with `h1_streaming_headers_build`, which asks again and
  sees the same latched verdict. Evidence: `h1/045`, and `h1/044` reads the two
  counters exactly rather than as floors, which is what would catch the double
  count the gate was there to prevent.

  Evidence: `h1/040` reads the 1.0 body with no framing around it against a 1.1
  control, `h1/041` proves the pipelined request is neither answered nor run,
  `h1/042` reads the keep-alive echo back and completes a second request on the
  socket, `h1/044` reads the drain's `Connection: close` off a streamed answer,
  `core/065` walks the bodiless-status contract from PHP on both paths and the
  `HEAD` dialect, `h1/045` proves a drain that comes due mid-stream still
  retires the connection, `h2/055` reads the frames HTTP/2 puts on the wire for
  each bodiless shape, `compression/075` decodes a gzipped close-delimited body
  and holds the three rules that meet on a declared 1.0 stream, `tls/016`
  drives the identity write branch over TLS with a chunk above the coalescing
  bound, `h1/046` refuses a header that would split the message and `h1/047`
  proves a handler `Connection: close` closes the socket, and `sendfile/004`
  refuses a status a file body cannot go under. Fourteen of the fifteen tests
  this step touches fail against `main`'s sources and pass here — the whole set,
  including the three that existed before it (`core/027`, `core/036`,
  `sendfile/004`). The exception is `h1/045`, which passes on `main` as well,
  because the defect it guards was introduced and removed inside this branch.

  Reviewed by four critics against the design before any code: 32 findings, 12
  survived verification, every one of them fixed above. The RST findings were
  refuted and are worth recording — a plain HTTP/1.0 request already closes the
  connection today, so close-delimited framing changes no byte of that path.

- [x] **#198 — an exception message reached the HTTP/1 status line unfiltered.**
  Merged in the same PR 199. Found while reading `emit_status_line` for the step
  above. A CRLF in the
  message ended the status line, so the bytes behind it were read as header
  fields and, past a blank line, as a second response (CWE-113) — reachable from
  any handler that puts request data into an exception message, and from
  `setReasonPhrase()`. The phrase now keeps only what RFC 9112 §4 allows;
  everything else becomes a space. Cleaned rather than refused, because
  `reset_to_error` runs while an exception is already being handled. Evidence:
  `h1/043`. Not addressed: the phrase still carries the whole message, so a 500
  puts application text of any length on the status line.

- [x] **#202 — the static path advertises `Connection: keep-alive` on HTTP/1.1
  too.** Three sites, not one: the served file (`send_file.c:455`), the inline
  error (`:236`) and the 416 (`:372`) all stated the field from the connection's
  verdict alone. The rule now lives in `h1_response_state_connection` beside
  `h1_response_peer_speaks_http11`, and all five callers read it — the three
  static ones with the real verdict, the two handler ones with `true`. The
  version check was written twice before and statics would have been the third
  copy. The wrong CHANGELOG claim went with the pass that shortened the whole
  `[Unreleased]` block. Evidence: `h1/048` reads the raw wire on four requests
  and fails against `main` on two of them.

- [x] **#200 — HTTP/2 and HTTP/3 strip `Content-Length` from every response,
  `HEAD` and static files included.** Answered A of the two readings: every
  buffered response states a count, rather than only the `HEAD` and the
  `sendFile()` whose framing cannot answer for itself. `sendFile()` is framed by
  DATA and END_STREAM as completely as any buffered GET and sat in the narrow
  answer for a download-sizing reason that holds for every body, so the narrow
  answer had no principle to stand on. The step was not four `false` arguments:
  the rules lived inside `http1_format.c`, and `head_streamed` inside
  `http_response_internal.h`, where no HTTP/2 or HTTP/3 site could read them.
  They are now `http_response_length_action` with four outcomes — state the
  buffer's count, state zero, keep the field the table holds, send none — and
  HTTP/1 reads it instead of its own copy. The one fact no call site had is a
  bit, `length_stated`: the static engine writes a count into the table while
  the buffer stays empty, so `http3_stream_submit_response` could not tell that
  count from a handler's. Two rules the reading missed: a response carrying
  trailers states nothing, because nghttp2 ends the stream at the byte
  completing the count and the trailing HEADERS then reaches it closed
  (`h2/003` caught it); gRPC needs no carve-out, since `writeMessage()` puts the
  response in streaming mode and the streaming rule already covers it.
  Evidence: `h2/056` and `h3/062` are new and fail against `main`; `static/012`
  now reads `cl=4096` on GET and HEAD, `h3/019` counts 72 headers. 455 phpt,
  429 passed, 0 failed, 24 skipped on absent tool gates, 1 warn (`core/047`,
  the pre-existing XFAIL that passes); `ctest` 16 of 16.

- [ ] **Measure what the framing work costs per response.** Not today; recorded so
  the number is taken before the next release rather than assumed. #195, #197 and
  #200 each added per-response work to a path that runs for every request. The one
  to measure first is #200 on HTTP/2 and HTTP/3: `http_response_commit_content_length`
  writes the count into the header table, so every buffered response now pays a
  `zend_hash_update` plus two `zend_string` allocations — the lowercased name and
  the formatted value — where the field used to be dropped and cost nothing. HTTP/1
  is untouched by that: it reads the action and formats into the smart_str as
  before. The `HEAD` branch adds one `zend_hash_str_exists`. Compare against the
  commit before #200 with `h2load` on a buffered response, same machine and same
  build, three runs, median, into `dev/BENCHMARKS.md`. If the insert shows, the
  next shape to try is handing the count to the flatten loop as an extra entry
  instead of through the table.

- [x] **#204 — an aborted request is logged and counted as the status it
  committed.** Answered by looking at what other servers do rather than by
  taste: nginx keeps `$status` and reports completion in `$request_completion`,
  Apache in `%X`, Envoy in `%RESPONSE_FLAGS%`, HAProxy in `termination_state`.
  None substitutes the status, so ours stays what the peer was told. The record
  is OTel Logs with semconv names, so the marker needed no invented key —
  `error.type` is conditionally required exactly when a request ends in an error
  and forbidden when it does not. `responses_aborted_total` joins the counter
  table, overlapping the four status buckets rather than joining them.

  The second column went with it. `written_length` turned out to count only a
  declared stream's bytes — the reserve was below the `declared < 0` return — so
  the plan's claim that it "holds the real number" was true for one case in two.
  It now counts every chunk, the audit still reads it only against a
  declaration, and `http.response.body.size` reports it. Evidence: `core/066`
  fails against `main` on all four of its lines, including a 204 logged with the
  four bytes the wire dropped. Not addressed: a `sendFile()` body never passes
  the response object, so its size stays 0, and a compressed stream reports what
  the handler wrote rather than the octets on the wire.

- [~] **Migration.** The CHANGELOG entries are written (#180, five bullets covering
  the seven renames, plus #181 under Fixed). What is left is laravel-spawn, and it
  is five call sites rather than the two this plan assumed: `send()` → `write()`
  at `src/Server/TrueAsyncServer.php:492`, `isClosed()` → `isEnded()` at 106, 395
  and 413, and `Sse::connected()` → `isWritable()` at `src/Sse/Sse.php:42`. To land
  once a build carrying the renames is tagged. Its docblock was
  corrected ahead of the rename in YanGusik/laravel-spawn#63, so the wording stops
  teaching the loop that truncated #60 in the meantime.

## HTTP/1 has no non-blocking write, and the two candidate fixes are both wrong

`tryWrite()` cannot refuse on HTTP/1: the streaming path writes through
`http_connection_send` → `send_raw`, which submits a `uv_write` and awaits it, so
backpressure is the kernel socket buffer and there is no depth to read. Two
designs were worked out and both fail on something mechanical.

- **A second writer for the non-blocking case** (`http_connection_send_batched`,
  the one WebSocket uses). It is an unordered channel: a chunk body parked in
  `out_pending_buf` waits behind an in-flight write while the headers, a blocking
  `send()` and `mark_ended`'s terminal chunk go out through the raw path and reach
  the peer first. Chunked framing does not survive that.
- **A queue on the response** (`http1_request_ctx_t`). Dead twice over: on TLS the
  drain writer would run in scheduler context, where `tls_wait_space` refuses
  outright (`src/core/http_connection_tls.c:97`), so it could not push a byte on
  HTTPS; and the context is freed in `http_request_finalize`, while a queue drained
  by write completions outlives the handler by definition. The precedents cited for
  it — the H2 per-stream ring and the wslay FIFO — both live on connection-lifetime
  objects, not on a per-request one.

- [x] **Measure the HTTP/1 chunk path before deciding.** Taken on 2026-08-20, on a
  release PHP built for it (`dev/BENCHMARKS.md`). `strace` confirms three `write(2)`
  and three loop turns per chunk, flat in the chunk size. A one-hunk prototype that
  copies the three pieces into one awaited write gains 42.7% at four chunks, 77.3%
  at sixteen and 84.8% at sixty-four, cutting the per-chunk cost from ~18.5 µs to
  ~8.8 µs. The copy is only there because the ABI has no awaitable vectored write:
  `io_pipe_writev_cb` sends no NOTIFY and frees the request itself.

  What it decides: the win needs no queue, no second writer and no per-response
  structure, so it is not an argument for #179.

- [x] **Send a streamed chunk as one write below 32 KiB.** Merged in #184. The
  threshold is where the copy stops paying, measured: +25% at a 32 KiB chunk, −16%
  at 64 KiB, so a larger frame keeps the copy-free three-write path. Verified
  against a 9% noise floor by alternating the two builds — +81% at 4 KiB chunks,
  and no difference at 64 KiB, where both take the same path. The bound is on the
  frame rather than the chunk: `HTTP_TLS_PLAINTEXT_RING_BYTES` is also 32 KiB, so a
  chunk-sized bound made a 32 KiB chunk spend a second ring cycle on a TLS record
  carrying six bytes, and a static assert now keeps the two from drifting.

  The header block rides inside the first frame when the two fit: +28.5% on a
  one-chunk response, which is the shape of an SSE reply. `mark_ended` checks
  `stream_dead` before its header commit, so a stream that died after its headers
  landed no longer sends them twice.

  The copy stays until ext/async grows a vectored write that reports its status.
  Its release callback carries none today (`zend_async_API.h:588`) and
  `io_pipe_writev_cb` destroys the exception before calling it, so a coroutine can
  be resumed from there but cannot learn whether the write failed — and on this
  path a failed write is what makes `isWritable()` honest (#176). The same change
  would close the buffer-lifetime hazard both branches carry: libuv keeps the
  caller's pointer until its completion callback, while a cancelled request is only
  marked pending.

- [x] **A cancelled write no longer outlives the buffer it points at.** Merged as
  server#186, on true-async/php-src#24 (+#25 into stable) and php-async#262.

  libuv does not copy: `uv_write` keeps the caller's pointer until its completion
  callback, and the caller freed it when *its own wait* ended — the same moment
  until a cancellation, after which the wait is over and the write is not.
  Reproduced with a peer whose `SO_RCVBUF` is 4 KiB and never reads,
  `setShutdownTimeout(0)` so `stop()` skips the grace window and cancels at once,
  and a streaming handler: the await returns incomplete, dispose defers because
  the write is in flight, and the caller frees a 20008-byte frame that the
  allocator immediately hands back to the next request. The disclosure itself was
  not demonstrated — the connection closes before the loop writes again — so this
  is a dangling pointer, not a shown leak.

  The ABI grew one bit: `ZEND_ASYNC_IO_WRITEV_AWAIT`, in the flag word the
  vectored write already had. The reactor keeps the request alive and notifies
  `io->event` with the request as the result and **no exception** — passing one
  would wake every reader and writer on the handle, because each listener
  forwards an exception unconditionally. On plaintext the HTTP/1 frame is now
  slots the reactor owns, so the body is not copied either; TLS keeps the copy,
  since `tls_push` copies into the BIO ring and a vectored write at the socket
  would put plaintext on a TLS connection.

  Two defects the same work uncovered: `h1_emit_headers_once` and the
  empty-first-chunk branch had the identical lifetime bug and now go through one
  helper; and `http_handler_coroutine_dispose` sealed a frame a cancellation cut
  in half, because it skips `mark_ended` and so skipped the `stream_dead` guard
  `4c7824c` added — the next request on that connection desyncs. macOS caught it
  where Linux could not: there the write still lands.

  Guarded by `ZEND_ASYNC_API_VERSION_NUMBER >= 0x001900`, not by the macro name.
  The name says the header knows the flag; only the version says the reactor
  keeps it, and a build pairing a new header with an old reactor hung — which is
  exactly what CI did in the window between the two merges.

- [x] **Cover the three files the #177 work left behind.** The coverage gate on
  #182 flagged a drop inherited from #178 — the compressing wrapper's four
  delegating ops, the pool worker's credit-backed answers, and the HTTP/1 branches
  around the header block. `compression/052`, the trio added to `h3/047`, and two
  more shapes in `h1/029` close it: compression 75.68 → 78.38, worker_dispatch
  76.63 → 79.62, http1_stream 66.23 → 71.43.

- [x] **Measure what a chunk costs before deciding.** Taken 2026-08-20, in
  `dev/BENCHMARKS.md`: one `writev` and one park per streamed chunk on plaintext HTTP/1,
  flat from 4 KiB to 64 KiB, and half a write with a seventh of a park on TLS, where the
  BIO ring already batches. The three submits the case rested on are one, so what a queue
  could still remove is the single park — and only by making the write fire-and-forget,
  which leaves `isWritable()` and `tryWrite()` with nothing honest to answer.
- [ ] **Answer from the queues the connection already has.** Plaintext:
  `out_pending_buf` carries a byte count, a high-water predicate on the same knob,
  low-water hysteresis, a drain hook and a destroy defer gate — all implemented and
  all exercised by WebSocket. TLS: `BIO_ctrl_get_write_guarantee` on the plaintext
  BIO is the exact predicate `tls_wait_space` loops on, so a refusal built from it
  is exact by construction. Neither needs a new structure. What it does need is the
  out-of-band writers brought under one order first — `send_strv_owned` ignores the
  pending tail, and `emit_parse_error` writes with a direct `send(2)` syscall.
  Measure before deciding: the case for it is three submits and up to three
  suspensions per chunk, and that number has never been taken.
- [ ] **#179 — one serialized outbound path per HTTP/1 connection.** The larger
  version of the same idea. Filed, and to be judged against the measurement rather
  than against the argument.

## Landed elsewhere

- **php-async #260 / #261.** Every debug CI job broke on 2026-08-20 with
  `op_array_emalloc_copy_array: Assertion 'zend_hash_num_elements(src) ==
  src->nNumUsed'`. The helper rebuilds two tables and asserted an invariant that
  holds for one: static variables need every bucket, because BIND_LEXICAL reaches a
  captured variable by a byte offset into arData, while a literal array may hold a
  hole — `[0 => 'a', 2 => 'b']` compiles to a packed hash whose slot 1 is UNDEF.
  The assert moved to the caller whose offsets depend on it. Server commit `5392ff7`
  was green at 09:11 UTC and the same commit failed at 12:50 with nothing changed on
  its side, which is what identified the cause.

## The cmocka suite rots unnoticed (#189)

Nothing built these targets in CI, so production signatures moved and the tests kept
compiling against the old ones. Two were repaired on 2026-08-19 (`ResponseWire`: the
`bool complete` argument dropped in 6fbe731 on 2026-07-07; `TLSSession`: the ring size
the backpressure case measures is the CT-in half, `TLS_BIO_RING_SIZE_SMALL`, not the
CT-out one). `ctest` ran 10 of 16 on 2026-08-20 and runs 16 of 16 now.

- [x] **Five targets do not link.** Closed by linking the real `src/grpc/grpc.c` and
  `src/log/trace_context.c` — both leaf TUs, so the code under test answers — and by
  stubbing only what no case reaches: the request-finalize tail for the parser targets,
  three symbols for the session target and twenty-one for the strategy one. Every new
  stub aborts instead of answering, which is what keeps a target that starts driving
  such a path from passing on an invented result. Evidence: 8 + 24 + 3 cases pass with
  every stub still aborting.
- [x] **`test_http3_packet` segfaults** in `test_account_send_error_buckets`. It was the
  test: its last line called `http3_packet_account_send_error(NULL, EAGAIN)` under the
  comment "NULL-safe, no crash", while the function opens with
  `ZEND_ASSERT(st != NULL)`. With asserts compiled out the null went through. The line
  is gone; the contract stays where it was written.
- [x] **Build the unit suite in CI.** In the embedded-fuzz job, the only one that builds
  a libphp for the suite to link against.
