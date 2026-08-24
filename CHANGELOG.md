# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.13.0] - 2026-08-24

### Added

- **A streamed body can be framed by a length the handler declares.** A `Content-Length` set before the first `write()` now reaches the client on every protocol and frames the body; it used to be dropped on all four paths, so nothing could size a download or check it against a promise. The server holds the body to the number: a write past it throws, a body that ends short is failed rather than ended cleanly, and such a response is not compressed. Evidence: `h1/037`, `h1/038`, `h1/039`, `h2/054`, `h3/060`, `h3/061`, `compression/074`.
- **A streaming handler can offer a chunk without waiting for room (#177).** `HttpResponse::tryWrite()` returns false when the outbound queue is full, having queued nothing, so the same chunk can be offered again; a client that has gone still throws `HttpException` 499, because "wait" and "stop" need opposite reactions. Mirrors `WebSocket::send()`/`trySend()` and shares `HttpServerConfig::setStreamWriteBufferBytes()`. HTTP/1 keeps no queue of its own and never refuses.
- **A refused chunk can be waited out instead of spun on (#177).** `HttpResponse::awaitWritable()` suspends until the outbound queue has room and reports whether it has; the only shapes after a `false` were a sleep-and-retry loop or a fall back to the blocking `send()`. A transport that can be full but cannot be waited on answers false, so a handler is never told to go ahead and spin without yielding.
- **The two dialects got the non-blocking half as well (#178).** `HttpResponse::trySseEvent()` and `tryWriteMessage()` frame exactly what `sseEvent()` and `writeMessage()` frame and answer false on a full queue, having committed no header. Without them `tryWrite()` was unusable from an SSE or gRPC handler, because the framing lives inside the dialect call.
- **A streaming response can be disowned instead of finished (#171).** `HttpResponse::abort()` ends a started stream as failed; a handler that threw halfway used to have its body sealed by the dispose path, so a 5 MB half of a 10 MB export answered `200 OK` with a clean end and `curl -o` exited 0. Each protocol says it the one way it can: HTTP/1 withholds the terminator and drops the connection, HTTP/2 and HTTP/3 reset the stream. Evidence: `h1/033`, `h2/053`, `h3/058`, `h3/059`, `grpc/018`.
- **A handler can ask whether the client is still there (#175).** `HttpResponse::isWritable()` reports whether output is still possible, and a false answer is final, which is what makes it safe to break on. The only predicate before it was `sendable()`, which also answers false on a full queue — the loop that truncated a proxied body at ~100 KB in YanGusik/laravel-spawn#60.

### Changed

- **A buffered HTTP/2 or HTTP/3 response is 3.6% faster to answer (#235).** The `Content-Length` the server computes went into the response header table, so every buffered response paid a `zend_hash_update` and two `zend_string` allocations for a field the flatten loop read straight back out and no PHP code reads at all. The count now reaches the wire as a header entry of its own, the way `:status` already did. Measured on `/b3` with `h2load -n 100000 -c 16 -m 16`, the two builds alternating inside each round with the order flipped every round: **95 ns per response**, faster in 15 rounds of 19, paired median ratio 1.0357 (`dev/BENCHMARKS.md`, 2026-08-24). The header set on the wire is unchanged.
- **BC: `write()` streams, and the buffered append moved to `appendBody()` (#180).** `write()` appended to a buffer and put nothing on the wire until `end()`, while Node, Swoole and Go all stream under that name. The first call now commits status and headers, so a later `setHeader()` or `setStatusCode()` throws where it used to work; rename buffered appends to `appendBody()`.
- **BC: `send()` is removed; the call is `write()` (#180).** No alias is kept: an alias would have covered one call in the shipped laravel-spawn adapter while `isClosed()` breaks three others beside it, so that adapter needs a release either way. A call now fails as an undefined method, at the line that has to change.
- **BC: `isClosed()` is now `isEnded()` (#180).** It returned the flag `end()` sets and reported nothing about the connection, so a handler reading it as "the peer is gone" got a wrong answer for the whole life of the response. `isWritable()` is the call that answers liveness.
- **BC: `isEnded()` answers true after `abort()` as well as after `end()` (#171).** It reports that the response is finished, not which call finished it, so `while (!$res->isEnded())` stops on a failed stream instead of writing into one.
- **BC: `sendable()` is removed, and its declaration is a tombstone (#180).** One bool answered four questions — closed, sealed by `sendFile()`, detached, full — and `README.md` documented it as a liveness check until #174. Calling it raises `HttpServerRuntimeException` naming both replacements; the declaration stays one minor release so shipped code is told what to call.
- **BC: `getBodyStream()` and `setBodyStream()` are removed (#180).** Neither ever had an implementation: the first returned null, the second threw. A handler wanting a file on the wire calls `sendFile()`, one wanting incremental output calls `write()`.
- **BC: `setStatusCode()` takes 200 to 599, and refuses an interim status (#197).** A 1xx was accepted and framed as a complete message, so a client read the header block and went on waiting for a final response the handler had no way to send — both ends then waited out a timeout. `setStatusCode(100)` now throws `HttpServerInvalidArgumentException`.
- **BC: `RoomDeliveryException` extends `HttpServerException`, not `WebSocketException`.** A build configured with `--disable-websocket` serves rooms, and in it `WebSocketException` does not exist. Catch `RoomDeliveryException` or `HttpServerException` instead; nothing else about the exception changed.
- **Rooms build without WebSocket.** The pub/sub core is `src/room/` and no longer knows what a connection is; `--disable-websocket` compiles it and delivers a publish from one thread to a `recv()` in another. The request-shutdown sweep that detaches a subscribed thread now runs in every build — without WebSocket it did not, so such a thread leaked its mailbox and a live libuv handle.

### Fixed

- **HTTP/3 under the reactor pool returned an abandoned request's slot from the wrong thread (#261).** The release picked its route by reading the listener through the stream's connection, and teardown NULLs that pointer while a worker still holds the request — so a stream whose peer went away took the single-thread branch and freed its slot into the reactor's slab from the worker: the freelist and the live count mutated off the owning thread, and the per-stream cleanup that the free assumes has already run was skipped, leaving the chunk queue, the response body, the borrowed request body and the wire credit unreleased. The route is stamped on the stream at creation now, the way its counters and log state already are, and only a closed pool — where the listener is gone and no reactor takes work — is reclaimed locally. Measured with a print in the branch: an abandoned request under the pool read `owned=1` and took the other route. The route is a named predicate now, and the `HTTP3SlotRelease` unit cases turn red on the old rule.
- **`StaticHandler` refused the prefix `/`, which is the document root every framework runner mounts (#259).** The constructor threw "url prefix must start and end with '/'" for an argument that does both: a length bound beside the bracket test read a one-character prefix as malformed. The resolver already took a one-character prefix as it stands — the whole path is then the relative one — so only the bound is gone. A mount at `/` with `StaticOnMissing::NEXT` serves the file from disk, hands an unmatched path to the PHP handler, and keeps a hidden `*.php` off the wire. Evidence: `static/022`, which fails against `main` at the constructor.
- **A `$response` kept past its request read the freed transport context (#256).** The response object carries the transport's per-request state behind `stream_ops`, and that state is freed when the request finalizes — so a handler that stashes `$response` in a global, a queue or a cache left the pair pointing at free memory, while `isWritable()` and `write()` refuse only on a closed response or a missing `stream_ops` and saw neither. Measured on HTTP/1: a handler keeps one response, 200 requests recycle the freed `http1_request_ctx_t`, and `isWritable()` on the kept object segfaults in `h1_stream_is_alive`. HTTP/1, HTTP/2 and HTTP/3 now clear the pair with the context, as the reactor pool already did, so such a call answers false and a late `write()` throws instead of reaching a stream that belongs to another request. Evidence: `core/070`, which dies with a signal against `main`.
- **Destroying a protocol strategy freed the strategy and kept what it owned.** `http_protocol_strategy_destroy` did one `efree`, while the HTTP/2 and WebSocket strategies each allocate a session lazily on first feed and release it from `cleanup(conn)` — reachable only through `conn->strategy`. A strategy that never reached a connection therefore leaked its session, measured at 184 bytes over two blocks by the `HTTP2Strategy` unit binary, and every caller had to remember the pairing to avoid it. A `dispose(strategy)` op now carries the release, `destroy` calls it, and `cleanup` routes to the same code. The two production call sites already paired cleanup with destroy, so nothing leaked on a live connection.
- **A peer that shut its write half down lost the rest of its response (#249).** Every terminating read latched the connection unwritable, and a clean EOF is what a half-close produces — so a handler streaming to a peer that had merely finished sending had its output dropped: over HTTP/1 the client got 110 bytes and no chunked terminator where 4 MiB were owed, over HTTP/2 between 65536 and 393216 DATA bytes of 4194304 and no END_STREAM. The read side now latches on a read error alone. The verdict a fire-and-forget write could not deliver comes from the handle instead: the reactor sets `ZEND_ASYNC_IO_WRITE_FAILED` on a failed write (TrueAsync ABI 0.26.0) and every write completion reads it, which keeps `trySseEvent()` and `tryWriteMessage()` answering 499 to a peer that is genuinely gone — the RST is consumed by the write, not by the read, so the read behind it returns a clean EOF and told nothing. Evidence: `h1/057`, `h2/064`, both failing against `main`; `h1/032` holds the other half.
- **HTTP/3 dropped a streamed response's trailers when the handler called `end()` (#247).** `end()` reaches `h3_stream_mark_ended`, which resumes the stream and drains, so the data reader can reach EOF inside that call; the trailers were captured afterwards, in the dispose, and the fin had already gone. A stream the dispose ended kept them, which is why the shape went unnoticed. Measured with the aioquic client, one trailer, four handler shapes: `end()` after the write lost it whether the trailer was set before the write or after, while a dispose-ended stream, a buffered response and every shape under `TRUE_ASYNC_SERVER_REACTOR_POOL=1` kept it; HTTP/2 kept all four. The capture now runs before the latch in `h3_stream_mark_ended`, the order `h3_stream_finish_streaming` already used for gRPC, and it is idempotent so the dispose-side call stays correct. Evidence: `h3/067`.
- **HTTP/3 stopped emitting response headers at 256 and dropped the Content-Length with them (#247).** The flatten loop left through a `goto` on the 257th field and reported nothing, so the fields the hash order had put last were gone — `content-length` among them — while the body went out in full, unframed. Measured with 300 headers and a 512-byte body: the direct path delivered 254 headers and no `content-length`, against 300 and the `content-length` from both the reactor pool and HTTP/2. The cap is removed rather than mirrored: it arrived with the initial import behind no issue, named its threat as "a server-side accident" rather than a peer, and bounded 10 KB of `nghttp3_nv` copied out of a `HashTable` that already holds the same strings at several times the cost. HTTP/3's real limit is the peer's `SETTINGS_MAX_FIELD_SECTION_SIZE`, which is a negotiated byte count and not this. Evidence: `h3/068`.
- **A handler killed by a bailout answered 200 over the body it had half-built (#244).** A `zend_bailout` longjmps out of the handler without unwinding it, so the response object keeps the status and the part-written body it held at that instant. HTTP/1 replaced those with a 500; HTTP/2, HTTP/3 and the reactor pool derived a status from `coroutine->exception`, which a bailout leaves NULL, and committed what the handler had left. Measured with a handler that sets a 200 and the body `half-built` and then exhausts an 8 MB `memory_limit`: HTTP/1 answered `500` with 21 bytes, while HTTP/2, HTTP/3 and HTTP/3 under `TRUE_ASYNC_SERVER_REACTOR_POOL=1` each answered `200` with the 10 bytes, `content-length` included. The same answers came back from a stack-overflow bailout under a 512 MB limit, so it is the longjmp rather than the allocation failure. A bailed-out gRPC call was reported as `grpc-status: 0` and is now 13. Every transport replaces an uncommitted response through one predicate, `http_response_reset_after_bailout`, which is the block HTTP/1 already carried. Evidence: `core/069`, `grpc/019`, `h3/064`, `h3/065`.
- **An HTTP/3 stream slot came back to a slab the listener had already freed (#245).** A stream lives in a slab slot, and the slot returns only when the PHP `HttpRequest` wrapper is collected — a handler may keep that wrapper in `$GLOBALS`, a queue or a cache, and a bailout keeps it too by abandoning the VM frame that held it. Listener teardown freed the chunks anyway, along with the listener the pool was embedded in, so a later return wrote `slot->list_next` into freed chunk memory and the counters into the freed listener. A debug build stopped at the live-slot assertion in `http3_stream_pool_cleanup` and dumped core; a release build wrote and carried on. The pool is allocated on its own now and outlives its creator: teardown closes it, and whichever comes last — the close or the last slot — frees the chunks and the pool together. The slab moved to persistent memory with it, because under the reactor pool the deferred return lands after the allocating thread's request heap is gone: on ZMM the same test reported `Total 1 memory leaks detected` and then `zend_mm_heap corrupted`. Evidence: `h3/066` keeps one request past `stop()` and reads it back; `h3/064` and `h3/065` could not reach their own assertion until this was fixed.
- **A worker pool reported nothing for twenty-two of its counters (#169).** The four timing fields moved into the counter slab; twenty-two more stayed plain members of the server object, so `getStats()` never listed them and a pool had no way to add them up — connections, parse errors, TLS handshakes and drain sweeps were each whichever worker answered the scrape. They are slab fields now, with a row in `HTTP_SERVER_COUNTER_TABLE` apiece, so `totals` sums them across the pool and `workers[]` keeps the split: `active_connections` as a gauge; `pause_count_total`, `codel_trips_total` and `paused_total_ns`; the seven TLS handshake counters, with the sum and the count separate so the mean is taken after the pool is summed rather than averaged from per-worker averages; the six parse-error counters; and the five drain counters. The three hooks that bump them take the counters slab their callers already hold, the way `http_server_on_tls_io` already did. Evidence: `telemetry/012` holds six connections open and offers six malformed requests across three workers, and reads both back from the pool's totals; `telemetry/009` pins the twenty-two new names into the reported key set.
- **`getStats()` read its per-worker breakdown and its totals in two passes.** A counter bumped between them landed in one and not the other, so under live traffic `totals` stopped being the sum of `workers[]`: six held connections came back as a total of 6 against a breakdown adding to 4, twice in fourteen runs. Both now come out of one traversal, which hands each slot's values to the breakdown from the same read pass that accumulates them.

- **A request body the server refused left the stream open until it timed out (#226).** `setMaxBodySize` is documented to end an over-large HTTP/2 body with a stream reset, and the check in `cb_on_data_chunk_recv` said so by answering `NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE` — a value that callback's contract does not carry, because `nghttp2.h` gives it `NGHTTP2_ERR_PAUSE` and nothing else. The refusal reached neither the wire nor the handler: no `RST_STREAM`, no status, and the handler still parked in `awaitBody()` with nothing else coming. Five refusals were spelled that way — the body cap on both the buffered and the streaming path, a queue push that failed, a WebSocket codec error, and the out-of-memory guard below. Each submits the reset itself now, the way the admission reject in `cb_on_begin_headers` already did, and answers 0 so the sibling streams live. The handler learns of it through `HttpException`, with 413 for the cap and 500 for the rest, and the reset is no longer counted as one the peer sent. Measured: 32 KiB against a 16 KiB cap put no frame on the wire and now answers `RST_STREAM(ENHANCE_YOUR_CALM)`; under `h2load -n 16 -c 4 -m 4` with 1 MiB bodies and `memory_limit=8M`, 12 of 16 requests timed out and 16 of 16 now finish, three runs of three. Evidence: `h2/061`, `h2/062`.
- **A caught bailout left the coroutine that resumed next without an execute frame (#226).** `_zend_bailout` clears `EG(current_execute_data)` and arms `gc_protect` before the longjmp, and `zend_end_try` gives back `EG(bailout)` alone. Fifteen firewalls in this server catch a bailout and keep running, because one failed stream must not take the connection and one failed request must not take the worker — and none of them restored either. The out-of-memory guard around the HTTP/2 body buffer runs on the C stack of whichever coroutine is driving the reactor tick, and a coroutine woken inside that same tick returns from its suspend without a context switch, so nothing else put the frame back. `HttpRequest::awaitBody()` and `HttpResponse::write()` are declared `static` and resolve that type through the frame, so the return took `E_CORE_ERROR` and the request was abandoned mid-response. Each firewall now snapshots the frame and the collector flag before `zend_try` and restores them first thing in `zend_catch`. Measured under `h2load -n 200 -c 20 -m 4` with 1 MiB bodies against the default 128 MiB limit: one such fatal per run before, three runs of three, and none after. Carried without a test: the window needs about two hundred concurrent uploads and opens once per run.
- **A peer that stopped reading held its connection for as long as the server ran (#223).** Queued bytes carry no await to time out, and none of the three batched senders armed the connection's write deadline — so a wedged writev kept `out_in_flight` set, the destroy deferred on it, and the deadline sweep re-deferred on every tick. Every plaintext HTTP/2 response and, since #209 and #211, every fire-and-forget HTTP/1 one goes out that way. The deadline is armed at each submit now, so it measures a write that stopped making progress rather than the lifetime of a response. Two defects came out with it: the timer closed the io itself, leaving the reactor's handle closed but never disposed — one 520-byte handle per timed-out connection, measured five for five — and on HTTP/2 nothing asked for the teardown at all, because the stream ends and the connection stays for multiplexing it can no longer do. The close belongs to the destroy, which is the one place that pairs it with the dispose. Evidence: `h2/060`.
- **A teardown could not get its last frames out from behind a write in flight (#225).** `http2_session_emit` skips while a writev is outstanding and relies on the completion to re-drive it, which a path that is about to destroy the connection will not be there for. `http2_session_emit_now` queues behind that write instead, and the write completion asks the session for what it still holds before finalising a deferred destroy. Evidence: `h2/064`, whose half-close defers the destroy behind a writev — with the re-drive back in the completion's tail it delivers all 1048576 body bytes and no END_STREAM, so the peer cannot tell a finished body from a cut one; 10 runs of 10 either way.
- **HTTP/2 over TLS alternated two drain cursors over one session (#224).** The hybrid emit picks DRAIN (`nghttp2_session_mem_send`) or GATHER (`nghttp2_session_send`) per pass from a counter that crosses zero between passes, and DRAIN parks the tail of a slice nghttp2 already considers sent. GATHER then resumes from the next frame, so the peer reads a truncated frame followed by a well-formed header — the shape #219 removed from the plaintext read path. A parked slice pins the mode now. The choice is a named function, `h2_tls_emit_use_drain`, so the unit suite reaches it: `test_emit_selector_parked_slice_outranks_the_mode` in `HTTP2Strategy` fails on the pin alone. What stays untested is the wire shape, which needs a frame-level TLS client the suite does not have — `_h2_client.inc` speaks h2c only.
- **A refused WebSocket upgrade answered out of order and then held the socket (#221).** The 4xx was written straight at the io, past the response still waiting in the connection's coalesce tail, so a client that pipelined three requests read the 426 as the answer to the second — measured `200,426,200` on three runs of three. It is queued now, behind whatever the connection already owes. Nothing asked for the teardown either: the 4xx says `Connection: close` and the socket was then held to the read deadline — 2.95 s at `setReadTimeout(2)`, 7.46 s at 6, 30 s by default — for one unauthenticated request. Both refusal paths retire the connection now, and the destroy waits for the queued write. Evidence: `websocket/070`.
- **One PING was answered with two ACKs (#222).** The server submits the ACK itself, because nghttp2 withholds its automatic one from a closing session and a gRPC shutdown sequence waits on that ACK — but nghttp2's was still on, so both fired: two ACKs on a live session and two on one the peer had GOAWAYed. A client measuring round-trip time from its own PING retires the exchange on the first and reads the second as an answer to a PING it never sent. nghttp2's automatic ACK is off now. Evidence: `h2/058`.
- **A large HTTP/2 response was truncated for a client that refills its window often (#219).** The h2c read path drained nghttp2 at the socket with `send(2)`, past the writev the emitter had already handed to the reactor. `http2_session_drain` takes the bytes out of the session, so whatever the socket refused was gone: a 3 MiB body reached a client acking every 16 KiB as 2686976 bytes with no END_STREAM, and a probe at the break caught 5376 bytes dropped from the middle of a DATA frame. What the socket did take went ahead of the queued response — the overtake #209 and #211 closed for HTTP/1. The read path now drives the same emit the response path uses: it queues on the connection and skips while a writev is in flight, because the completion re-drives it. The bad-preface GOAWAY, which nghttp2 does not know it owes, is queued directly; the protocol-error GOAWAY goes through the emit like everything else, because two cursors over one session leave a half-copied frame nothing comes back for. A teardown cannot wait for the write completion to re-drive the emit, so it queues behind the write in flight instead — that is what keeps the GOAWAY of a session that finished while a response was going out, which the verdict at the end of the feed used to lose because it is read only when an inbound byte arrives and a peer told GOAWAY sends none. Two more the review found: the emit freed its record array and handed off its buffer without detaching either from the state a bailout cleans up, and a chained submit that never reached the reactor left the in-flight flag set, stranding the destroy deferred on it. Evidence: `h2/057`, and `h2/059` pins the bad-preface GOAWAY at the one frame it has always been.
- **`awaitWritable($ms)` waited out the write deadline on a pool worker.** The slot defines a nonzero argument as the caller's own bound, and the worker transport read it and then parked on the configured deadline instead — seconds, on a thread carrying other requests. The argument is threaded into the credit wait now; zero still asks for the deadline. Evidence: `h1/054` pins the contract; the worker path itself is uncovered.
- **A streamed HTTP/2 response stated a length the buffered one refused.** The buffered commit drops `Content-Length` when the response carries trailers — with a stated count nghttp2 puts END_STREAM on the DATA frame that completes it — while the streaming commit asked only whether a length was declared, so one response was framed two ways. Both read the same rule now. Trailers set after the first `write()` are past the header block and keep the length. Evidence: `h2/056`.
- **A pool's worst-case latency dropped to zero when the workers rotated (#169).** A retiring worker folded only its monotonic totals into the registry, so a peak died with the worker that measured it. A peak is carried now, the way a total is. `h2_ping_rtt_ns` keeps the old behaviour under a kind of its own: it is the latest sample rather than a peak, and a dead worker's round-trip time is stale. Evidence: `telemetry/006`.
- **A worker pool could not report request latency, and did not even collect it (#169).** The four timing fields were plain members of the server object, so four workers held four unrelated averages and no API summed them; `setTelemetryEnabled(true)` was also dropped when the config crossed into a worker thread, so every worker's sample count stayed 0. They are counters now: `getStats()['totals']` carries the pool's sums and the worst latency any worker saw, `workers[]` keeps the per-worker split. Evidence: `telemetry/011`.
- **`tryWrite()` never refused on HTTP/1, and blocked instead (#179).** Every offer answered true and parked the handler inside the write, so the "refused — do something else" branch was dead code on the most common protocol. A non-blocking offer is now queued and refused once the outbound depth — the coalesce tail plus the write in flight — reaches `setStreamWriteBufferBytes`. `write()` keeps the awaited path, so the write still reports a peer that left. Evidence: `h1/053`.
- **A streamed HTTP/1 response overtook a buffered one still queued ahead of it (#211).** #209 fixed the fire-and-forget senders; the awaited ones submitted at the io just the same. Four pipelined requests, two buffered and two streamed, came back `S1,W3a,S2,W3b,W4a,W4b` on five runs out of five. The awaited senders now wait for the tail, so the write still reports a peer that left. The wait costs a scheduler turn on every awaited send that lands behind batched bytes, and `setWriteTimeout` now bounds the queue wait together with the write. TLS orders through the BIO ring and is unchanged. Evidence: `h1/052`.
- **Pipelined HTTP/1 responses came back out of order when a large one followed a small one (#209).** The dispose picks its sender by body size, and only the small-body one honoured the connection's write ordering — the other submitted straight at the io, past the write in flight and past the tail waiting on it. Three pipelined requests answered with 2, 2 and 1800 bytes came back 1, 3, 2, and a pipelined client, which matches responses to requests by order (RFC 9112 §9.3.1), read another request's body for two of the three. Both owned-buffer senders now queue through the same path as the batched ones. The 4xx a parse error answers with had the same escape — built in the read path and written at the socket, it landed between the responses to the valid requests parsed out of the same buffer, so `200, 200, 400` read as `200, 400, 200`; the plaintext side of it now queues too, while TLS keeps the BIO ring it always used. HTTP/2 and HTTP/3 order their own streams. Evidence: `h1/049` and `h1/051`.
- **The access log reported 0 bytes for a large `sendFile()` body and the unencoded size for a compressed stream (#206).** `http.response.body.size` was read off the response object, and neither shape passes its bytes through it: a file past the 64 KiB slurp threshold goes from the descriptor to the socket, and a codec re-sizes every chunk the handler wrote. The same server therefore logged the encoded size for a buffered body and the plaintext size for a stream, and nothing at all for a static download. Each transport now reports what it put on the wire — the three file pumps and the compressing wrapper — and the HTTP/1 kernel path, which had no count of its own, adds the bytes its `sendfile` op transferred. Evidence: `core/067`.
- **An aborted response was logged and counted as one that finished (#204).** `http_request_telemetry` read the status the handler had committed before it failed, so a CSV export that died after 5 of 10 MB produced one `responses_2xx_total` and an access line no different from a whole one. The status stays what the peer was told — nginx, Apache, Envoy and HAProxy all keep it and carry the abort in a separate field — and the record gains OTel's `error.type`, set only on a request that ended in an error. A new `responses_aborted_total` makes the truncation visible in the counters. In the same record `http.response.body.size` counted the buffered body, so a 204 or a `HEAD` was logged with bytes the wire had dropped and every streamed response with zero; it now counts what the transport was handed. Evidence: `core/066`.
- **A file response advertised `Connection: keep-alive` to an HTTP/1.1 client (#202).** The file engine stated the field from the connection's verdict alone at three sites — the served file, the inline error, the 416 — while the two handler paths have asked the peer's version since #197. Persistence is the default on HTTP/1.1 (RFC 9112 §9.3), so the field said what the peer already assumed, and RFC 9110 §7.6.1 asks a sender not to generate a connection option it does not need. One rule answers for all five callers now; a 1.0 client that asked to keep the socket still gets the echo. Evidence: `h1/048`.
- **HTTP/2 and HTTP/3 sent no `Content-Length` on any response, `HEAD` and static files included (#200).** The header filter dropped the name on every path and nothing computed one to put back, so a `HEAD` answered with no size and a 4 KiB `sendFile()` gave the client nothing to size the download by. The server now states the count it computed — buffered body, file size, range length — and nothing where nothing is known: an undeclared stream, a `HEAD` that streamed, a 1xx, a 204, a response with trailers. Evidence: `static/012` reads `cl=4096` where it read `cl=-`.
- **A streaming response to an HTTP/1.0 client was framed by chunked coding it cannot read (#197).** `Transfer-Encoding: chunked` went out whatever version the request used, so a 1.0 client read `5\r\nalpha\r\n4\r\nbeta\r\n0\r\n\r\n` as twenty-six body bytes where nine were sent. Such a body is now delimited by the connection close (RFC 9112 §6.3 rule 7). A streaming response also never advertised `Connection: close` at all, because the header was set in the dispose path long after the block had left.
- **A status that carries no body was sent one (#197).** `setStatusCode(204)->setBody('oops')` sent `Content-Length: 4` and four bytes, so the next response on the connection was read from inside them. A streaming call on such a status now throws while the response is uncommitted; a buffered body is dropped at format time, because the status may legitimately have been chosen after the body was built.
- **The buffered path sent `Content-Length` beside a handler `Transfer-Encoding` (#197).** A handler that set the header got it copied onto the wire next to the server's own count, which RFC 9112 §6.1 forbids and §6.3 names as the shape a request is smuggled through. The handler's `Transfer-Encoding` is dropped now: on both paths the framing is the server's to state.
- **An exception message reached the HTTP/1 status line unfiltered, so a CRLF in it split the response (#198).** `throw new RuntimeException("bad path: $path")` put request data between the status code and the CRLF that ends the status line, and a `\r\n` in it turned one 500 into a 500 with an injected header plus a 200 the server never sent (CWE-113). The phrase now keeps only what RFC 9112 §4 allows there; every other byte becomes a space, and the body still carries the message as written.
- **`sendFile()` and `json()` were two more doors onto the status code (#197).** Both wrote `status_code` past the check `setStatusCode()` makes, so `json($data, 100)` produced the interim response that hangs the exchange and `new SendFileOptions(status: 204)` framed a whole file under a status the client ends at the blank line. All three callers ask one predicate now. Evidence: `sendfile/004`.
- **A 205 Reset Content carried a body, and a body it could not frame (#197).** RFC 9112 §6.3 rule 1 does not name 205, so a client still looks for framing and read the next response as this one's content. The body is dropped on both paths and the emptiness is stated as `Content-Length: 0`. Evidence: `core/065`.
- **A `HEAD` whose handler streamed reported `Content-Length: 0` (#197).** `write()` dropped the chunk and returned before the stream committed, so the length was computed from a buffer nobody had filled — which states that the `GET` body is empty. The response commits as a streaming one now, dropping only the bytes, and states no length it never measured. Evidence: `core/065`.
- **A handler `Connection: close` was copied to the client and ignored by the server (#197).** The field went on the wire while `conn->keep_alive` stayed true, so the server answered the next request on a socket the client had already retired. The field is read rather than copied now: `close` retires the connection on HTTP/1, `keep-alive` is dropped as what the server was going to say anyway, and any other value throws. Evidence: `h1/047`.
- **A header name or value reached the HTTP/1 wire unchecked, so a CRLF in one split the response (#198).** `setHeader('Location', $next)` copied whatever the handler passed, and a `\r\n` in it made the bytes behind it read as further fields and, past a blank line, as a second response (CWE-113). `setHeader()`, `addHeader()`, `redirect()` and `setTrailer()` now throw `HttpServerInvalidArgumentException` naming the offending byte and its offset. Evidence: `h1/046`.
- **A handler `Content-Length` that disagreed with a buffered body desynced the connection.** `setHeader('Content-Length', '5')` with an eleven-byte body sent all eleven under a header naming five, and on a keep-alive connection the client read the remaining six as the next status line. The server states the count it is sending; the handler's number is kept only on `HEAD`, where it describes the body a `GET` would return.
- **A buffered body was discarded without error when the handler then streamed (#181).** `setBody()` followed by a streaming call put only the streamed chunks on the wire, while the reverse direction had always thrown. The guard every streaming entry point shares now refuses a non-empty buffer and names both modes. Evidence: `core/062`, route `/mixed`.
- **A cancelled handler could seal a half-written chunk (#177).** An HTTP/1 chunk is three writes and the coroutine suspends between them, so `mark_ended` wrote the terminal zero-chunk over an orphaned size line and handed the connection on for reuse, while the peer read that terminator as the promised chunk's data. Such a frame is now recorded as a dead stream: no terminator, and the connection is not kept alive.
- **A dropped chunk was reported as written (#177).** When the reactor's mailbox refused a wire after its retries, `worker_stream_append_chunk` answered OK, so a pool-dispatched handler was told it had written bytes the peer will never see. It reports the stream dead now.
- **`sendable()` answered a constant true under compression, and on HTTP/3 (#177).** The compressing wrapper carried no `sendable` slot and a NULL slot means "report writable", so a refusal on HTTP/3 threw away a block deflate had already emitted and the retry wrote those bytes into a window that held them. Both now report from the transport that owns the queue.
- **A streamed response was held by the compressor until the stream ended (#170).** Every chunk was fed to the encoder in continue mode and a block closed only by `finish()`, so 350 KB of CSV emitted in five bursts 300 ms apart arrived as one 10 KB burst after 1.5 s under `Accept-Encoding: gzip`. The encoder vtable gained a `flush` op, called once per non-empty chunk the handler hands over. Cost: 7.7 bytes per chunk for gzip, 9.9 for Brotli, 9.8 for zstd over 80 chunks of 51 bytes (`dev/BENCHMARKS.md`).
- **A compressed stream whose handler forgot `end()` was undecodable (#171).** `end()` went through the wrapper that closes the codec stream, while falling out of the handler reached the transport underneath, so `gzdecode()` refused a body complete by every framing rule it could check. Every dispose path finishes through the response's own ops now. Evidence: `compression/071`, `compression/072`.
- **A stream reset by the server could be reported as reset by the peer.** nghttp2 delivers a locally sent RST_STREAM through the same close callback as a remote one, so a handler that called `abort()` had its own error replaced by `HttpException` 499 and the reset counted in the peer-reset metric. Both exclude a self-sent reset now.
- **A pipelined HTTP/1 request behind a broken stream was still answered (#171).** The peer was still counting down the bytes an unfinished chunk had promised it, so the next response arrived as that chunk's data — a whole `HTTP/1.1 200 OK` read as body. It goes unanswered now, on an aborted stream and on a cancellation that cut a frame in half.
- **A pool worker's abort could still reach the client as a clean end.** Raising the stream's end flag before the tick's flush let the data reader emit EOF before the reset, telling the peer the body was whole — observed in one run out of three. The pending bytes are drained before the flag goes up, so the peer also keeps the part of the body the handler did produce.
- **A compressing stream wrapper returned a faulted encoder to the pool.** The streaming path left an encoder that answered `HTTP_ENC_ERROR` attached to the response, and teardown handed it back to the per-thread pool for the next response to reuse. It is destroyed on the spot, and `mark_ended` writes no trailer when the encoder is gone.

## [0.12.0] - 2026-08-15

### Added

- **A room can be received from, not only published to (#152).** `Room::subscribe()`, `Room::recv()`, `Room::unsubscribe()` and `Room::lostCount()`. A topic's subscriber used to be a WebSocket connection and nothing else, so code running in a pool thread could publish into a room but never hear a stop command or an answer; the topic tree now carries server-side receivers beside sessions, through the same nodes, tombstones and pruning. A subscription belongs to a thread and a handle, and a room handed to a pool task arrives unsubscribed and subscribes for itself — `recv()` without a subscription throws rather than answering null, because a caller that cannot tell "no message" from "never joined" waits forever for something that was never routed to it. A second concurrent `recv()` on one subscription is refused.
- **A `Room` transfers into another thread (#150, #151).** A room holds a hub reference and a snapshot of the three reliable-send knobs, instead of pinning the `HttpServer` that minted it, so carrying one into a pool task no longer carries the listeners and the handler graph with it. Before this, a task closure that captured a room received an uninitialized object. `HttpServer::room()` still mints before `start()` and creates the hub when the server has none; a running server without a hub is refused, because its workers attached to nothing and a publish would reach no tree.

### Changed

- **A reliable send reports what it reached, and refuses when it reached nothing (#156).** `delivered` counted remote mailboxes only, so a send whose whole room sat on the calling thread answered 0 having served it, and a send with no thread attached to the hub answered success with `delivered` 0 — for a control message, a stop command that vanished. `delivered` now counts local subscribers, the fan-out reports live worker slots and `publish()` surfaces them as `workers`, `send()` refuses whenever it reached no target at all, and `trySend()` answers false there. Both status enums name a cancellation, so five PHP call sites stop re-reading `EG(exception)`. The drainer cadence belongs to the hub, taken at `create()` and copied at attach: it used to be read per call and applied once per attachment, first send winning.
- **One message body per publish, whatever the node holds (#155).** A publish copied the body once per server subscriber on the node, and a cross-thread publish copied it again on arrival although the command already carried a shared persistent copy. Measured over ten publishes to four subscribers in a pool thread: 52 bodies allocated before, 11 after.

### Fixed

- **`Async\graceful_shutdown()` did not stop a server at `setWorkers(1)`, the default configuration.** Above one worker `start()` delegates to the pool and awaits it, so cancelling that await ends the parent and the workers are told to stop. A single worker runs the standalone path instead, where the only teardown is `stop()`: it releases the listen events, which closes their libuv handles. Cancellation woke `start()` without going near that, so the listeners stayed armed on the loop, `uv_run` always had a live handle, and the process ran forever — the thread sat in `epoll_pwait` with an infinite timeout, not deadlocked, just never told to stop. `start()` now performs the same teardown when it is woken while still running. Measured: 10 hangs in 10 runs before, 10 clean exits in 10 after; `stop()` at one worker and `graceful_shutdown()` at two are unchanged. Test `tests/phpt/server/core/061-single-worker-graceful-shutdown.phpt`, which fails by timeout without the fix.
- **A process running an `HttpServer` alongside an `Async\ThreadPool` deadlocked on the way out, and at four workers corrupted the heap instead.** The topic hub is created by the owning server and shared with every worker clone, but it was freed by the owner's object destructor — which the shutdown of a userland `ThreadPool` reliably brings forward, ahead of a worker still in the epilogue of `start()`. That worker then called `tsrm_mutex_lock(hub->admin)` on a block already returned to the allocator: printing the mutex showed `__lock = -1027408845` and `__kind = -163562720`, values a `pthread_mutex_t` cannot hold. The main thread waited for those workers in `libuv_reactor_quiesce()` and never got them. The hub now carries a reference count, and every copy of the pointer takes one where the copy is made — the worker shell as the hub is fanned out, the clone as the shell is loaded — so a reference is always derived from one the copying thread already holds. Taking it at first use instead would only move the same race earlier, into the window between a clone reading the pointer and the worker attaching. Whichever holder leaves last frees the hub. Measured with `setWorkers(2)`: 10 hangs in 10 runs before, 12 clean exits in 12 after; with `setWorkers(4)`, 5 aborts with `corrupted double-linked list` in 10 runs before, 12 clean in 12 after. A worker whose waker cannot be created now detaches on its way out of `start()`, which it did not do before: that path already stranded the slot and the mailbox, and with a reference count it would strand the hub itself.
- **A thread whose only coroutine waited on a room was cancelled as a deadlock 0.4 ms after it parked (#153, #158).** A room's wake source is the thread inbox trigger, which does not hold the loop open by itself, so the thread looked idle. The park now holds a reference on the inbox for its duration, and that reference belongs to the event the coroutine parks on rather than to the C frame that parked: a frame runs its epilogue only if it unwinds, and a fatal error longjmped past the release, leaving the worker with a live handle and the scheduler aborting the process on "The event loop must be stopped". `start()` takes the reference, `stop()` gives it back, and `dispose()` gives it back if `stop()` never came.
- **A request that cannot run PHP is torn down without waking anyone (#154).** The room teardown had one shape — wake whoever is parked, and let the last reference drop what is queued — and both halves are wrong once the request is over: after a fatal error the coroutines parked on a room are gone while their waiter events are still request memory, and the reference they held on a subscription is never dropped. Measured: 64 persistent payloads per dead worker. The sweep at request shutdown, and `start()`'s own bailout, now unlink the attachment before the drain, wake nobody, and release by hand what the absent owners would have released. A `subscriberCount()` reply drained after its asker died is disposed of instead of firing an event owned by a dead request.
- **A cancelled `recv()` dropped the message it was cancelled beside (#152).** A cancellation landing in the same turn as a delivery popped the message off the ring and returned without it, because the caller is unwinding: nothing was counted anywhere and the publisher was told it had been served. The pop no longer happens while an exception is pending, so the subscription outlives the cancelled coroutine and the next reader takes the message. `Room::recv()` also passed a negative timeout through to a C convention where negative means "no deadline", so an expired computed deadline parked forever; only `null` waits without one.
- **A transferred room leaked the hub reference and the topic it carried (#151).** The transit shell's C state was unreachable at release — `thread_release_transferred_object` frees the properties table and the allocation and never `free_obj` — so a room handed to another thread dropped neither. It is released through the new transfer kind (`ZEND_OBJECT_TRANSFER_RELEASE` in php-src, dispatched by php-async), which this release requires. `HttpServerConfig` releases its frozen snapshot the same way; `HttpServer` does not, because it owns its shells and frees them itself. Measured: definitely lost falls from 21,648 bytes in 2 blocks to zero.
- **Uploads use the temporary directory PHP was configured with (#160).** The multipart parser named `/tmp` itself and never read `upload_tmp_dir`. Naming a directory is also what makes the core fall back and report the fallback, and the parser runs in an event-loop callback with no PHP frame above it: the `E_NOTICE` reached a user error handler that throws — Laravel installs one — and the exception had nothing to unwind to, so the request died without a response while requests around it were served. The call mirrors `main/rfc1867.c`: `upload_tmp_dir` where it is set, `NULL` otherwise so the core picks the system temporary directory, and `PHP_TMP_FILE_SILENT` so a directory that cannot be used costs a fallback rather than a dropped request. A temporary file that cannot be created at all is still reported, through `MP_UPLOAD_ERR_NO_TMP_DIR`.

## [0.11.3] - 2026-08-13

### Fixed

- **A bootloader that threw made `HttpServer::start()` answer `true` for a server that never accepted a connection.** The pool rejects every worker's submission before its request loop starts, and the parent counted those rejections as workers that had finished their work, so the run read as a clean start followed by a clean stop. `start()` now answers `false` when any worker was rejected, and the parent names the first reason on stderr (`worker did not start: …`). The exception itself is printed by the worker, with `display_errors` and `error_log` applying as usual — that half needs `true_async` 0.9.2.

## [0.11.2] - 2026-07-15

### Fixed
- **`enableWebSocket(true)` claimed WebSocket "is not yet implemented" (#134).** WebSocket has been fully implemented for a long time; like `enableHttp2()`, this toggle is deliberately rejected because the working path is `addWebSocketHandler()`. The exception now says "Use addWebSocketHandler() to enable WebSocket" instead of lying about missing support. Behaviour is unchanged — WebSocket is enabled by registering a handler.

## [0.11.1] - 2026-07-15

### Fixed
- **MSVC build broke on the new log-sink `open` member (Windows x64 Release/Debug).** `win32_compat.h`'s `#define open(path, flags, ...) php_win32_ioutil_open(...)` textually rewrote `type->open(spec, &opened[n], &mode)` in `http_server_class.c`, since the preprocessor matches on the `open(` token regardless of the preceding `->`. The call is now parenthesized (`(type->open)(...)`) to suppress the macro expansion at this one call site; the macro itself is unchanged and still covers real `open()` calls elsewhere.

## [0.11.0] - 2026-07-15

### Added

- **Cross-worker WebSocket topics (#2).** `WebSocket::subscribe()`,
  `unsubscribe()`, `getTopics()`, `publish()`, `publishBinary()` and
  `subscriberCount()`. A publish reaches subscribers on every worker of the
  process; until now it could only reach peers of the sending worker (a worker is
  a thread with its own PHP context), so a chat had to run `setWorkers(1)`.
- Topic filters follow MQTT: `+` is exactly one level, `#` is the rest — so
  `chat/+/typing` or `user/42/#` are subscriptions. A publish topic must be
  concrete. Membership lives in the connection, and a topic is a string at the
  call site: there is no topic object to obtain, hold or pass into a handler.
- Share-nothing: each worker matches publishes against its own topic tree, and a
  session pointer never crosses a thread. `subscriberCount()` is a
  scatter/gather over the workers, so it is a snapshot, not a live counter.
- Topics work on every WebSocket transport, not just plaintext HTTP/1: over TLS,
  over HTTP/2 Extended CONNECT (where a session is bound to a stream, so a
  publish reaches the sibling streams of the publisher's own connection), and
  with permessage-deflate — one `publish()` serves a compressed peer and a plain
  one side by side, each with the framing it negotiated. A publish to a peer whose
  socket is backed up drops the message rather than queueing it, and never
  suspends, so one dead reader cannot stall delivery to everyone else.
- **Interest filter on publish.** Each worker summarises its subscriptions in a
  counting Bloom filter of topic prefixes, and a publisher skips the workers that
  cannot match, instead of waking all of them. A publish to a topic nobody in the
  process listens to now costs zero cross-worker wake-ups instead of one per
  worker; wildcard subscribers are still always reached.
- `HttpServer::getRuntimeStats()` reports `ws_topic_posted`, `ws_topic_skipped`
  and `ws_topic_dropped`.
- `HttpServerConfig::setWsMaxSubscriptions()` caps the distinct topic filters one
  connection may hold. Default 0 — no limit, the same default every self-hosted
  broker ships (EMQX `max_subscriptions`, NATS `max_subs`), because only the
  application knows how many topics it needs. Over the cap the filter is refused
  and the connection stays up, as EMQX answers with SUBACK 0x97 and NATS with
  `-ERR 'Maximum Subscriptions Exceeded'`.
- **`HttpServerConfig::setWsPublishRateLimit()` — a leash on `publish()` (#120).**
  Per-connection token bucket, off by default (as EMQX ships `messages_rate`).
  `publish()` is the one WebSocket call an unprivileged peer can turn into work on
  *every* worker in the process — `send()`/`trySend()` only ever touch its own
  socket. Unmetered, one client looping on a relayed message fills every worker's
  inbox, and the drops that follow take out *other* topics' traffic too. Over the
  rate `publish()` throws `WebSocketBackpressureException` and the connection
  stays up: the sender is told, rather than the message vanishing into a full
  mailbox where nobody can see it.

- **Observability — telemetry metrics + logging redesign (#5).** Metrics are
  read through a plain PHP array (no embedded exporters); logs fan out to
  pluggable sinks.
  - **Cross-worker stats (`HttpServer::getStats()`).** Opt-in via
    `HttpServerConfig::setStatsEnabled(true)`. Each pool worker owns one slot in
    a process-wide, cache-line-aligned counter slab and bumps it lock-free (no
    atomics on the hot path); `getStats()` walks the slab from any thread and
    returns `{enabled, workers, totals}`. Totals include `total_requests`,
    per-status-class `responses_2xx/3xx/4xx/5xx_total` (each request classified
    exactly once, so the four sum to `total_requests`), and per-protocol active
    gauges `conns_active_h1/h2/h3`. Throws when stats are disabled.

    Counters carry their aggregation kind, so a value is combined the way its
    meaning allows: monotonic totals sum and **survive a `reload()`** (a
    retiring worker's totals are inherited, so a scraper never sees a counter
    run backwards just because the pool rotated); active gauges sum across live
    workers only (a dead worker holds no open connections, so its last value is
    not carried forward as a phantom); and a latest-sample counter such as
    `h2_ping_rtt_ns` reports the maximum, since summing four workers' round-trip
    times describes nothing.
  - **Multi-sink logging (`HttpServerConfig::setLogSinks()`).** A log record now
    fans out to several sinks at once, each with its own severity floor and
    formatter; the fast gate is the minimum floor across sinks, and one failing
    sink (drop-counted) never blocks the others. Emit formats once per distinct
    formatter before fan-out. `setLogSinks([['type'=>'stream'|'stdout'|'stderr',
    'stream'=>$res, 'format'=>'plain'|'logfmt'|'json'|'pretty',
    'level'=>LogSeverity::…], …])` declares up to 8 sinks (invalid specs throw at
    config time); `setLogSeverity()`/`setLogStream()` stay as single-stream sugar.
    Each sink writes through the stream's own async IO handle and batches records
    in a per-sink ring buffer flushed at a 32 KiB high-water mark or a 200 ms
    timer, so a burst of logs coalesces into far fewer write syscalls while the
    emit call itself never blocks.
  - **syslog sink — TCP, UDP and unix datagram.** `['type'=>'syslog',
    'target'=>'tcp://host:port' | 'udp://host:port' | 'udg:///dev/log',
    'facility'=>'local0', 'level'=>…]` ships records as RFC 5424 messages. The
    formatter emits the bare message; framing belongs to the transport: TCP
    gets RFC 6587 octet-counted framing (a receiver splits records even when a
    message carries an embedded newline), while UDP and unix-datagram targets
    send exactly one record per datagram so message boundaries survive. PRI
    packs the configured facility with the severity mapped to syslog levels.
  - **Structured access log (`'category' => 'access'`).** A sink spec may
    carry `'category' => 'app' | 'access' | 'all'` (default `'app'`): `app`
    receives server diagnostics, `access` receives exactly one structured
    record per completed request — so a JSON access log and a pretty
    diagnostics console coexist on one server. Attributes use the stable
    OpenTelemetry HTTP semantic conventions, matching the OTel Logs envelope
    the `json` formatter already emits: `http.request.method`, `url.path`,
    `url.query`, `http.response.status_code`, `network.protocol.version`
    (`"1.1"`/`"2"`/`"3"`), `http.response.body.size`,
    `http.server.request.duration` (seconds, per the spec's unit),
    `client.address` (bare IP) and `client.port`, plus the W3C trace context
    when telemetry parsed one — so a collector can interpret the record
    without a custom mapping. Emitted on every completion path (handler
    return, static handler, compression reject, sendFile engine, reactor-pool
    worker dispatch) across HTTP/1, HTTP/2 and HTTP/3, including under a
    worker pool. Off by default: without an access sink the cost is one
    predicted branch per request. The text formatters (plain, pretty, template,
    syslog) escape control bytes in values as `\xXX`, so a request-derived field
    cannot forge a log line or inject an ANSI sequence into a `pretty` console;
    json and logfmt already quote their own output.
  - **No sink calls back into PHP, by design.** Records are emitted from libuv
    IO-completion callbacks, connection teardown, error paths and the HTTP/3
    reactor threads — a bare thread pool with no TSRM context — so the logging
    path must not re-enter the VM. To export logs from userland, point a sink
    at a file or socket with `'format' => 'json'` and drain it from your own
    coroutine; that is the async-appender shape, and it also keeps exporter
    latency off the request path and gives you batching. Extensions register
    native destinations through `http_log_register_sink_type()`.
  - **`file` sink + logging now works under the worker pool.** New
    `['type'=>'file','path'=>…]` sink: each pool worker reopens the path
    itself (append mode). Previously NO logging configuration crossed into
    pool workers at all — the frozen config snapshot dropped it; sink specs
    are now flattened into the snapshot and rebuilt per worker, so
    `file`/`stdout`/`stderr`/`syslog` sinks (and the access log) work with
    `setWorkers(N)`. `stream` (a parent-opened resource) cannot cross threads:
    it stays active on the parent and is skipped in workers with a start-time
    notice.
  - **Sink-type / formatter registry (plugin seam).** `setLogSinks()` resolves
    `'type'` and `'format'` names through a registry instead of hardcoded
    lists; another extension can add its own sink type or formatter at MINIT
    via `http_log_register_sink_type()` / `http_log_register_formatter()`
    (built-ins register through the same seam). Validation error messages list
    whatever is actually registered. The `syslog` wire format is not a public
    formatter — it carries no record framing (that is the syslog transport's
    job), so it is reachable only through the `syslog` sink type, and
    `'format'=>'syslog'` on any other sink is rejected at config time.
  - **`template` formatter — user-controlled line layout.** `['format' =>
    'template', 'template' => '{ts:Y-m-d H:i:s.v} [{level}] {msg}{attrs}']`
    renders each record through a custom template: `{ts}` (ISO-8601) or
    `{ts:PATTERN}` with a PHP `date()`-style subset (`Y y m d H i s v`),
    `{level}`, `{msg}`, `{attrs}`, `{trace}`, `{span}`; everything else is
    literal (unknown placeholders pass through verbatim). The template is
    compiled once when the sink starts, so the per-record render stays a flat
    segment walk. Bad templates throw at `setLogSinks()` time.
  - **Formatters: `plain`, `logfmt`, `json`, `pretty`.** `json` is one
    OTel-Logs object per line (Timestamp/SeverityNumber/SeverityText/Body/
    Attributes/TraceId/SpanId, RFC 8259 escaping); `logfmt` is `key=value` with
    quoting; `pretty` is a coloured console line
    (`HH:MM:SS.mmm  LEVEL  message  key=val …`) whose colour is decided once at
    sink build from the target fd, honouring `NO_COLOR` / `CLICOLOR_FORCE`.

- **`HttpServer::stop()` works in pool mode (#117).** Calling it on a pool
  parent (`setWorkers(N)`, `N > 1`) used to throw. It now retires the whole
  cohort and **blocks until the server is really down** — when it returns, the
  workers have drained, the pool is torn down and the listen sockets are closed.
  This is what makes a graceful `Async\signal(SIGTERM)` → `stop()` shutdown work
  under the built-in worker pool. A standalone server's `stop()` keeps its old
  non-suspending behaviour: it is typically called from a request handler, and
  the shutdown drain waits on that handler — a blocking `stop()` there would be
  waiting for itself.

- **Client address on the request — `HttpRequest::getRemoteAddress(): ?string`
  and `HttpRequest::getRemotePort(): ?int`.** `getRemoteAddress()` returns the
  **bare IP** — no port, no brackets around an IPv6 literal — the same shape as
  `$_SERVER['REMOTE_ADDR']` (RFC 3875 §4.1.8) and as Servlet, Node, Swoole and
  ASP.NET return. The port is a separate accessor. A combined `"ip:port"` string
  is deliberately not offered: it is Go's documented wart, and it leaves callers
  unable to split an IPv6 address. `null` on a Unix-socket listener, which has
  no IP peer. The value is the socket peer and is **not** derived from
  `X-Forwarded-For`.
  `WebSocket::getRemotePort(): ?int` is added to match.

### Changed

- **BREAKING — `WebSocket::getRemoteAddress()` now returns the bare peer IP**
  (`"203.0.113.7"`), not `"host:port"` / `"[host]:port"`, and returns `null`
  rather than `""` when there is no IP peer. Use the new `getRemotePort()` for
  the port. This makes the method mean the same thing as the new
  `HttpRequest::getRemoteAddress()` instead of two different things on two
  classes.

### Changed

- **Pool workers are commanded, not polled (#117).** `reload()` used to bump a
  shared epoch that each worker read from its deadline-watchdog tick, which
  forced that timer to fire at least once a second per worker even when every
  timeout was disabled. The parent now reaches its workers over a control
  channel — an atomic command plus one `uv_async` wakeup per worker — so
  `reload()` and `stop()` land immediately instead of within a tick, and the
  deadline watchdog is back to doing only its own job (its cadence again follows
  the read/write/keepalive timeouts, up to 120 s when all of them are off).

  The command is state rather than an edge, which also closes a latent hang: a
  worker thread that started up *after* the epoch was bumped snapshotted the new
  value and would never retire, leaving the parent waiting on it forever.

- **Log sinks are owned by a dedicated log thread (#5).** A sink's descriptor,
  its write in flight and its flush timer now live on one consumer thread, and
  every other thread — pool workers, transport reactors, the parent — is a pure
  producer: it formats the record on its own stack and copies the bytes into its
  *own* ring for that sink. One writer and one reader per ring, so the emit path
  takes no lock and no atomic read-modify-write; publishing the write index is
  the whole synchronisation. The flush policy is unchanged (32 KiB high-water or
  the 200 ms timer), and so is the drop-on-overflow behaviour.

  This is what finally lets a transport reactor log. Under the reactor pool a
  `sendFile()` is delivered by the reactor, so the reactor is the only place the
  final status exists — it was already counted there, but the access record was
  lost, because a reactor has no PHP context and must never touch a worker's
  descriptor. It can fill a ring.

  Sinks are still built per server clone, so under a worker pool each worker
  still opens its own (and still cannot open a `stream` sink — a PHP resource
  does not cross threads; use `file`). What changed is that the parent's sinks
  are now reachable from the transport reactors, which is what the access record
  needed. Sharing one sink set across the pool — one descriptor per sink instead
  of one per worker — is a follow-up the log thread makes possible but does not
  yet do.

- **Per-request stream state is smaller (#122).** The `bool` flag walls on
  `http3_stream_t` (15 flags) and `http2_stream_t` (12) are now one bitfield
  block each, sitting next to `refcount` so they fill padding instead of adding
  it. The structs go from 864 → 832 and 744 → 704 bytes; a listener that keeps
  its stream slabs warm holds that much less per concurrent request. Both structs
  are written only by their own thread (an H/3 stream is driven by its
  connection's reactor; the worker never dereferences it, it only carries the
  pointer back on the response wire), so the flags may share a storage unit. No
  behaviour change.

### Fixed

- **`stop()` crashed when a subscriber was still connected (#2).** The worker let
  go of its topic tree before the scope drain destroyed the sessions — and a
  session unsubscribes itself as it is destroyed, so the teardown walked freed
  nodes. SIGSEGV on any stop() with a live subscriber.
- **Topics were unusable past 64 workers (#2).** `setWorkers()` allows 1024 but
  the hub had 64 slots, so workers beyond that got no topic tree: `subscribe()`
  threw and `publish()` quietly did nothing. The slot table matches
  `setWorkers()` now, and a worker that still cannot attach fails to start rather
  than serving half a feature.
- A connection reaches its topic hub through its own **server** now, not through
  a thread-global — per-server state belongs in the server (CODING_STANDARDS §1.2).
- Topic filters may be 128 levels deep, up from 32 — the ceiling EMQX uses.
- **A full worker mailbox dropped topic traffic silently.** Publishes that cannot
  be handed to a worker are counted (`ws_topic_dropped`) instead of vanishing.
- **A pool cohort outliving a cancelled parent (#117).** `Async\graceful_shutdown()`
  cancels the pool parent's await while its workers are still serving, and nothing
  in the engine stops a *busy* worker — closing the pool's task channel only reaches
  one that is back at `recv`, and ours is inside `start()`. The parent used to walk
  away and free the shared reload beacon under their feet; the workers then read the
  freed memory, got a value that differed from the epoch they had snapshotted, and
  self-stopped — which is why shutdown appeared to work. Now the parent commands the
  cohort to retire before it leaves, and the control channel is refcounted so it
  outlives every worker that is still reading it.

- **A log sink whose receiver stopped reading broke process shutdown (#121).** With
  a stalled peer (a socket nobody drains, a wedged syslog collector) the sink's
  write never completed, so the log thread's final drain waited on it forever: the
  transport stayed open, the thread never left its loop, and the process aborted on
  `ZEND_ASYNC_REACTOR_LOOP_ALIVE() == false && "The event loop must be stopped"`.
  The stop-time drain is now bounded on the log thread itself — past the deadline
  the write in flight is abandoned and the transport torn down regardless, so the
  thread always leaves. The abandoned write keeps its buffer until the reactor
  reports the cancellation (a file write is still reading it in the blocking pool),
  and the stopping thread's existing budget goes back to being what it was meant to
  be: a backstop, not the thing that has to fire. Records already in the ring are
  still flushed when the receiver is healthy; drop accounting is unchanged.

- **An exception in a WebSocket handler killed the worker thread, silently (#119).**
  The WS handler coroutine never consumed its exception, so it was rethrown at
  finalize and retired the whole worker — one bad connection cost a thread of
  accept capacity, and after N connections the port stopped accepting anything,
  including plain HTTP. The same hole existed on the HTTP/2 Extended CONNECT path
  (RFC 8441), and a fatal error (`memory_limit`, uncatchable error) took the worker
  down through the missing bailout firewall. A failing handler now behaves like a
  failing HTTP handler: the exception is consumed and logged with its class,
  message and origin, and the failure is reported to the peer in-protocol — an
  HTTP status when the handler threw before the upgrade committed (`Throwable::$code`
  when it is a valid 4xx/5xx, else 500), a `CLOSE 1011` frame when the session was
  already live. The worker keeps serving. Cancellation (server stop, connection
  teardown) is unchanged — it is not a handler failure.

- **One departing HTTP/3 peer could silence the listener for good.** When a client
  vanished while the server was still sending to it, the ICMP port-unreachable that
  came back was queued on the listener socket by `IP_RECVERR` and reported by epoll
  as `POLLERR` — and libuv answers `POLLERR` by removing the descriptor from the
  loop permanently. The listener then sat in `epoll_wait` while datagrams piled up
  unread in the kernel: every later HTTP/3 request to that process timed out. The
  poll is now re-armed (and the error queue drained) whenever this fires; the new
  `poll_rearms` counter in `getHttp3Stats()` reports how often it happened.

- **A static file or `sendFile()` over HTTP/3 could stall forever mid-body.** The
  body sender is callback-driven, but `h3_stream_append_chunk` still took its
  coroutine backpressure path when a chunk did not fit the congestion window —
  suspending inside a libuv callback on the transport thread, which never returns.
  The transfer stopped at the read-ahead ceiling (~64 KiB) with the peer's ACKs
  arriving and nothing left to wake the sender. It surfaced whenever the reactor
  fell behind the queue (a busy or single-core host), so it hit the reactor pool
  most often. The sender now keeps its own backpressure and append never suspends
  on its behalf.

- **A syslog sink over TCP wrote blockingly.** Every log descriptor was wrapped as
  an async *file*, so its writes went to the blocking IO pool — the same pool that
  serves `sendFile()`. A syslog receiver that stopped reading therefore parked a
  pool thread per write until its TCP window drained. The io type is now taken
  from the descriptor itself: an inet stream socket is driven as a socket, an
  AF_UNIX stream through libuv's pipe handle, everything else stays a file.
  Datagram sinks stay on the file path deliberately — a `send()` to a UDP or
  unix-datagram peer does not wait on a slow receiver, so there is nothing to
  unblock.

- **A static mount under the reactor pool read files synchronously on the
  transport thread.** The reactor never installed the protocol's streaming ops on
  the response it built, so the send-file engine found no operation to delegate to
  and fell back to reading the whole file — blocking the reactor loop (and with it
  every other connection that thread owns) and buffering the file in memory
  whatever its size. The ops are installed now, so a static hit goes through the
  same callback-driven body pump as everything else.

- **Log records dropped by a full ring were invisible.** A sink's ring is bounded
  on purpose — the producer must never block — so a burst that outruns the writer
  costs records. They were only ever mentioned in a rate-limited stderr line.
  `getStats()` now reports `log_records_dropped_total`, counted per producer
  thread (each charges its own counter slice, so the bump is race-free and the
  loss is attributed to whoever produced it). An observability feature that
  silently loses log lines is worse than one that admits it.

- **HTTP/1 streaming responses were never counted.** `$response->send()` bumped
  `stream_send_calls_total` and `stream_bytes_sent_total` but not
  `streaming_responses_total` — HTTP/2 and HTTP/3 count it on their first chunk,
  HTTP/1 did not, so a server streaming over HTTP/1 reported zero streaming
  responses while its byte counters climbed.

- **`sendFile()` was counted and access-logged with the wrong status (#5).** The
  handler only *queues* a send and returns with the default 200; the real wire
  status (206, 304, 416, 404, 500) is stamped later by the send-file engine. But
  counting and the access record happened in the handler-coroutine's entry tail,
  so every ranged download was reported as a 200. Both now happen once per
  request, at each protocol's teardown — where the request and the response are
  both alive *and* the status is final — through one protocol-agnostic seam,
  `http_request_telemetry()`. It replaces five near-identical access-log wrappers
  and the send-file engine's own counting, which had the further flaw of
  resolving a worker's log sink from the transport thread.

  Requests served entirely on a transport reactor (a static hard-zero hit, the
  reactor half of a pooled `sendFile()`) are counted into that reactor's own
  counters, which `getStats()` now folds into `totals` and reports under a new
  `reactors` key — before, they were counted where nothing ever read them.

- **HTTP/3 range requests returned the wrong bytes.** A ranged `sendFile()` (or
  ranged static file) answered `206` with correct `Content-Range` headers and
  then sent the body **from the start of the file**: the H3 body pump stored the
  engine's `body_offset` and never seeked to it. HTTP/2 had always seeked.

- **HTTP/3 files larger than 64 KiB never arrived under the reactor pool.** Only
  the inline fast path (files up to the 64 KiB slurp threshold) worked; anything
  that needed the real body pump — every larger file, and *every* ranged request
  — hung until the client timed out. The pump drove itself from a PHP coroutine
  and demanded a server object and an async scope, and a transport reactor has
  neither (no PHP runs there), so it failed immediately and no response was ever
  sent. It is now a callback FSM, like HTTP/2's, and runs on any thread: reads
  complete into a callback that queues the chunk and decides whether to read on,
  with backpressure from the stream's write window and the per-thread static
  memory budget. As a consequence the reactor no longer touches the worker's
  request at all — the conditional headers the engine honours (`Range`,
  `If-Range`, `If-Modified-Since`, `If-None-Match`) now travel on the SEND_FILE
  wire, because those request fields live in the worker's allocation domain and
  freeing them from the reactor corrupts its heap.

- **Observability review fixes (#5).**
  - **Per-protocol connection gauge underflowed for every HTTPS connection.** A
    TLS/ALPN connection installs its strategy in the handshake path, which
    bypassed the `conns_active_h1/h2` increment while the close path still
    decremented — so the gauge wrapped toward `UINT64_MAX`. The handshake path
    now increments to match.
  - **A long request target corrupted the JSON/text access log.** A record that
    overflowed the formatter buffer was truncated including its trailing
    newline, merging it with the next record on a stream sink (a client could
    hide an unrelated request behind a long URL). The record separator is now
    forced even on truncation.
  - **`level => LogSeverity::OFF` sinks came back to life under a worker pool.**
    The frozen-config round-trip had no OFF case and collapsed OFF to INFO, so a
    sink the user disabled started logging in workers. OFF now round-trips.
  - **Pool-mode access records lost `http.server.request.duration`.** The worker
    stamped its service window on the dispatch ctx but the access record reads
    the request; the window is now copied across.
  - **The static hard-zero serve path counted requests but never logged them.**
    The send-file engine resolved its log state from a NULL server on that path;
    the h1/h2 worker path now passes its server so the access record is emitted
    (the transport reactor still passes NULL — its sinks belong to another
    thread).
  - **`resetTelemetry()` zeroed the live occupancy gauges**, so the next
    connection/stream close decremented past zero and underflowed. Reset now
    preserves `conns_active_*`, `active_requests` and `h2_streams_active`.
  - **Windows build:** the new relaxed 64-bit counter reads used the
    GCC/Clang-only `__atomic_load_n` with no MSVC fallback in TUs compiled on
    Windows; they now go through a portable helper.

- **`getHttp3Stats()` could return counters from two different moments.** The
  QUIC counters were read by copying the whole per-listener stats block while
  the reactor thread kept writing it, so fields on either side of an update
  could land in the same report — `quic_packets_sent` from after a send,
  `quic_bytes_sent` from before it. Each counter is now loaded individually with
  a relaxed atomic read, so the report is internally consistent. The counter
  list is also driven by a field table guarded by a static assert, instead of a
  hand-kept block of ~60 appends that a newly added counter could silently miss.

- **`WebSocket::trySend()` silently dropped frames over HTTP/2 (#2).** The H2
  chunk ring is bounded by slots as well as bytes, and the non-suspending sink
  discards a frame past the last slot — yet `trySend()` still returned `true`,
  so a handler looping on it lost every frame past the ring with no signal
  (measured: 100 accepted, 8 delivered, on a healthy reader too). The transport
  now exposes a `sendable` hook that the non-blocking path gates on, so
  `trySend()` reports BUSY as documented. HTTP/1 was unaffected.

## [0.10.1] - 2026-07-10

### Fixed

- **Windows build broken by the gRPC code.** `src/grpc/grpc.c` included POSIX
  `<strings.h>` unconditionally (for `strncasecmp`), which MSVC does not ship
  (`fatal error C1083`). The include is now guarded with `#ifndef PHP_WIN32`
  like the rest of the tree — on Windows `strncasecmp` comes from PHP's
  `zend_config.w32.h`. Linux/macOS are unaffected.

## [0.10.0] - 2026-07-10

### Added

- **gRPC over HTTP/2 and HTTP/3 (#4).** Requests whose content-type begins with
  `application/grpc` route to the callable registered via
  `HttpServer::addGrpcHandler()`; everything else is untouched, so gRPC and
  regular HTTP handlers coexist on one listener.
  - **All four RPC shapes** — unary, server-streaming, client-streaming and
    true full-duplex bidi: `HttpRequest::readMessage()` deframes the request
    stream incrementally (5-byte length-prefix framing, 16 MiB per-message cap),
    `HttpResponse::writeMessage()` frames replies; the handler starts on
    HEADERS, before the body finishes.
  - **Trailers**: `grpc-status`/`grpc-message` ride real HTTP trailers on both
    transports (nghttp2 trailer HEADERS; `nghttp3_conn_submit_trailers` at true
    EOF on H3 — verified with a real aioquic client). `grpc-status: 0` is
    defaulted on success, `13 INTERNAL` on an uncaught handler exception, and a
    handler that writes no messages gets the canonical Trailers-Only reply.
  - **grpc-web (binary)**: `application/grpc-web` calls carry their trailers
    in-body as a `0x80`-flagged frame, on H2 and H3.
  - **gzip message encoding**: inbound `grpc-encoding: gzip` messages inflate
    transparently in `readMessage()`; `setGrpcEncoding('gzip')` declared
    before the first `writeMessage()` compresses every reply frame (per-call
    declaration, mirroring grpc-go/java/C++ — a compressed frame without a
    declared encoding cannot be expressed).
  - **`grpc-timeout`** request header parsed and exposed via
    `HttpRequest::getGrpcTimeout()`.
  - **grpc-web-text**: `application/grpc-web-text` calls carry base64 both
    directions — `readMessage()` decodes the request transparently, every
    response frame (messages + the trailer frame) goes out independently
    base64-encoded.
  - **Works under the reactor pool** (`TRUE_ASYNC_SERVER_REACTOR_POOL=1`) —
    gRPC rides the generic streaming reverse path below; no gRPC-specific
    code in the reactor/worker split.

- **Reactor-pool streaming reverse path (#80).** Under
  `TRUE_ASYNC_SERVER_REACTOR_POOL=1` a worker response is no longer
  buffered-only:
  - `send()`/`writeMessage()`/SSE stream across the thread boundary — the
    worker posts STREAM_HEADERS / STREAM_CHUNK / STREAM_END wires in FIFO
    order; the reactor feeds its existing chunk ring and submits native
    trailers at true EOF (so `setTrailer()` works under the pool, buffered
    or streamed).
  - **Credit-based backpressure**: a per-stream credit block (atomics,
    malloc-domain) paces the producer — over 1 MiB un-acked in flight the
    handler coroutine parks and resumes as the QUIC peer acknowledges
    bytes, so a slow client cannot flood the shared reactor mailbox. Peer
    RST / connection close unparks it into the standard stream-dead path
    (`send()` throws 499).

- **HTTP/3 streaming request bodies (#26 policy on H3).** With
  `setBodyStreamingEnabled(true)` the H3 dispatch now applies the same
  three-case Content-Length policy as HTTP/2, so `readBody()` and
  incremental `readMessage()` (true full-duplex gRPC) work over HTTP/3.
  QUIC flow-control credit is deferred: the window refills as the handler
  drains chunks, bounding un-read bytes by `max_body_size`.

- **WebSocket permessage-deflate honours `server_max_window_bits` (RFC 7692).**
  The negotiation response now reflects the client's window-bits offer instead
  of always advertising the maximum, so peers that cap the window interoperate.

### Fixed

- **HTTP/3 uploads larger than the initial stream window (256 KiB default)
  stalled forever.** `nghttp3_conn_read_stream`'s consumed count excludes
  DATA payload by contract, and nothing extended the QUIC windows for
  buffered body bytes — now `h3_recv_data_cb` returns the credit as it
  consumes them.

- **Three latent use-after-frees found under ASAN** (masked by the Zend
  arena in normal runs): the WebSocket dispose read `w->committed` after
  the zval dtor could free `w`; `http_log_server_stop` awaited a write
  request that its own completion callback frees (now drains by yielding
  to the reactor and re-polling); the WebSocket reject / spawn-fail paths
  freed a request the H1 parser still borrowed via `parser->request`.

- **Duplicate request headers are now combined per RFC 9110 §5.3** across
  HTTP/1.1, HTTP/2 and HTTP/3 (shared `http_request_store_header`; `Cookie`
  joined with `"; "`, others with `", "`), instead of the last value winning.

- **Inbound WebSocket message FIFO is bounded** (8× `max_message_size`); a peer
  that floods faster than the handler drains is closed with 1013 rather than
  growing the queue without limit.

- **Reactor-spawned listeners get their own per-listener counters slice**,
  fixing a telemetry race where sibling reactors shared one dummy counter.

### Performance

- **HTTP/3 reactor-pool hot paths** (reactor review follow-up): reactor
  commands travel the mailbox by value (no malloc/free per message),
  O(1) intrusive stream unlink, listener local sockaddr cached per peer
  family, thread-local cipher context in CID steering, and a per-worker
  memory budget for H3 static delivery (ported from H2). Hard
  backpressure on a full worker inbox now RESETs the stream with
  `H3_REQUEST_REJECTED` instead of silently dropping the request.

- **HTTP/3 reactor-mode request bodies are built persistent from the first
  byte** and the wire body is adopted on the reactor instead of re-copied,
  removing a per-request copy on the reactor↔worker path.

- **gRPC `readMessage` reassembly compaction is amortized** instead of shifting
  the buffer on every frame, and assorted hot-path micro-cleanups from the
  transport audit (getenv cache, spin-pause, cached `getHeaders`/`getBody`).

### Changed

- **Configure fails fast on a non-ZTS (NTS) PHP** — the threaded worker pool
  requires ZTS/TSRM by design; the build now errors early instead of producing
  a broken extension.


- **gRPC layering: call-lifecycle policy extracted out of the transports (#4).**
  `src/grpc/grpc_call.c` owns response defaults, outcome → `grpc-status` and
  delivery shape (grpc-web in-body frame / streaming EOF / Trailers-Only);
  HTTP/2 and HTTP/3 provide a 3-op wire vtable and stay gRPC-agnostic on
  delivery. H3 response-trailer capture/submit is now generic — any streaming
  response with a trailer map is delivered, not just gRPC (parity with H2).

## [0.9.3] - 2026-07-07

### Fixed
- **Clean pool shutdown no longer leaks or crashes (#93).** On `Async\graceful_shutdown()` the pool parent now disposes the per-worker completion futures it owns from `submit_internal()`, so their cross-thread wakeup triggers no longer linger armed on the parent reactor (loop-alive assert on debug / leaked libuv handle on release). Also fixes a use-after-free from double-disposing the parent's `all_done` wait event (the waker already disposes it) — a hard crash on macOS ARM64 release. Requires php-async ≥ 0.7.9 for the race-safe `remote_future_dispose`. New tests: `055`–`057`.

### CI
- Disable `opcache.protect_memory` in the phpt suite — its process-global `mprotect` races across the threaded worker pool (false SIGBUS); it defaults off in production and real compilation is serialized by the SHM lock.
- Collect and upload symbolicated crash backtraces on test failure (macOS DiagnosticReports).

## [0.9.2] - 2026-07-03

### Added

- **`HttpServer::reload()` — hot reload of the worker pool without dropping the
  listen sockets (#93).** Pool-parent only. Bumps a shared epoch beacon
  (`pemalloc`'d, parent-owned, fanned out to worker clones through the transit
  shells); workers watch it from the deadline tick and retire themselves from a
  fresh main-scope coroutine (drain in-flight → `stop` → exit to the closed pool
  channel). The pool then rotates via the ThreadPool ABI `reload()` — replacement
  threads re-run the bootloader, so changed code is picked up — and one `start()`
  task per worker is resubmitted onto the fresh channel. Suspends until the old
  cohort has fully drained. Reload is serialized (one rotation at a time) and the
  lifecycle is logged (`reload.start` / `reload.done`, per-worker
  `server.stop reason=reload`).
- **Built-in hot-reload triggers (#93).**
  - `HttpServerConfig::enableHotReload(array $watchPaths, array $extensions = ['php'], int $debounceMs = 300, int $maxHoldMs = 2000)` —
    dev trigger: the pool parent spawns one recursive `Async\FileSystemWatcher`
    per path; a debounced change event invalidates the watched trees in opcache
    and calls `HttpServer::reload()`.
  - `HttpServerConfig::enableReloadOnSignal(bool $enabled = true)` — prod trigger:
    the pool parent arms a persistent `SIGHUP` handler that calls
    `HttpServer::reload()`.

- **Reload under the reactor pool (#93).** `HttpServer::reload()` now works with
  `TRUE_ASYNC_SERVER_REACTOR_POOL=1`. A worker-inbox retirement protocol
  unpublishes the retiring slot (admin mutex; picks stay lock-free), fences every
  reactor so no pre-retire inbox pointer survives, and a zero-crossing decrement
  wakes the worker to free its inbox — connections homed to the slot re-home on
  their next request. Slots are reclaimed so a rotated pool can re-register.

### Fixed

- **~10 KB leaked per reload rotation (#93).** The worker transit shell's C-state
  and side-cars were not released when the old cohort exited; now freed after the
  rotation completes.
- **Heap corruption on rotation under the gated reactor pool (#93).** A dying
  worker clone's free path unconditionally tore down the *global* worker registry
  and `g_reactor_pool` from the worker thread while the parent's reactors were
  live; the catch-all teardown is now parent-only.
- **macOS build: TSRM mutex instead of `uv_mutex` in the worker registry (#93)** —
  macOS TUs have no libuv include path.

### Changed

- **Test suite: kernel-allocated ports across every phpt (#93).** All phpt now bind
  to an OS-assigned port instead of a fixed one, eliminating the port-collision
  flake class (previously seen on the `h2/012` + `core/051`/`052` cluster).
- **Ctrl+C signal-delivery test harness (#94)** for macOS/Linux, covering
  interrupt, `SIGTERM`, `pcntl`-before/after, open-connection, multi-waiter, and
  dev-server scenarios.

## [0.9.1] - 2026-07-02

### Fixed

- **Static-build header discovery.** Canonical flat `php_true_async_server.h`
  registration header so `genif`/static builds resolve the extension header; added
  a flat `php_server.h` shim and renamed `http_server_module_entry` →
  `true_async_server_module_entry`. WebSocket server 0.9.0 feature set unchanged.
- **`htons`/`ntohs` declared for the bundled wslay under strict C99 compilers**
  (clang / zig-cc), fixing the WebSocket build on those toolchains.

## [0.9.0] - 2026-07-01

### Added

- **WebSocket support (RFC 6455) (#2).** Full-duplex over HTTP/1.1
  Upgrade, `wss://`, and HTTP/2 Extended CONNECT (RFC 8441), with permessage-deflate
  (RFC 7692). `HttpServer::addWebSocketHandler()`; `WebSocket` / `WebSocketMessage` /
  `WebSocketUpgrade` classes, `WebSocketCloseCode` enum, exception hierarchy.
- **Pull API** — `recv()` and `foreach ($ws as $msg)` (`WebSocket` is an `Iterator`);
  a graceful close ends the loop, an error close throws `WebSocketClosedException`
  carrying `$closeCode` / `$closeReason`.
- **Multi-producer send** — `send()` / `sendBinary()` safe from any coroutine, plus
  non-blocking `trySend()` / `trySendBinary()` and `WebSocketBackpressureException`
  under sustained backpressure.
- **Keepalive** — server-initiated ping (`ws_ping_interval_ms`) and a pong deadline
  (`ws_pong_timeout_ms`) that closes an unresponsive peer with 1001.
- **Outbound auto-fragmentation** — messages larger than `ws_max_frame_size` are
  split into continuation fragments no larger than the cap.
- **Conformance & fuzzing** — Autobahn|Testsuite runner (`e2e/autobahn/`, built from
  source in Docker, 246/246 on `behavior`) wired into CI, plus a wslay frame-ingress
  libFuzzer harness (`fuzz/fuzz_ws_frame.c`).

### Fixed

- **UTF-8 fail-fast no longer lingers the socket (#2).** On a protocol error wslay
  queues a CLOSE but `wslay_event_recv` still returns 0, so the handler stayed parked
  in `recv()` and the TCP hung forever once the peer echoed the close; now detected
  via `wslay_event_want_read()` and torn down.
- **Handshake-reject paths no longer leak the parsed request (#2).**

### Performance

- **One write per WebSocket frame (#2).** Frame header and payload were written
  separately (two `write()` syscalls per frame); coalesced into one — 51% fewer
  write syscalls and ~43% higher echo throughput under load.

## [0.8.1] - 2026-06-28

### Fixed

- **SSE/streaming: a client that aborts mid-stream no longer crashes the server (#3).**
  When the peer sent a RST, the next write's `uv_write()` failed at *submit* and the
  reactor left an `Async\AsyncException` ("Failed to start stream write: broken pipe")
  in `EG(exception)`. The awaiting send path (`http_connection_send_raw`) returned
  failure without absorbing it — unlike a *completion* failure, which
  `async_io_req_await()` already clears, and unlike the fire-and-forget writers, which
  call `http_absorb_io_submission_exception()`. The orphaned exception then surfaced
  with no PHP frame (`#0 {main}`) as an uncaught fatal, taking down every connection.
  The submit-failure branch now absorbs it too, so a dead peer reaches the handler as
  the canonical, catchable `HttpException` (499 "stream closed by peer"). New phpt
  `025-h1-sse-client-disconnect` reproduces the crash (RST mid-SSE) and asserts the
  499 instead.
- **H3 static-file pump now absorbs a read-submit failure too (#3).** The same
  asymmetry on the file-read side: when `ZEND_ASYNC_IO_READ` failed at submit, the
  producer coroutine broke out of the pump loop without clearing the reactor
  exception it left in `EG(exception)`, which would then surface as an uncaught
  fatal on unwind. It now absorbs it (the completion-error case was already
  handled via `req->exception`), keeping error handling symmetric across the
  write and read submit paths.

## [0.8.0] - 2026-06-27

### Added

- **Server-Sent Events API (#3).** First-class `text/event-stream` helpers on
  `HttpResponse` — `sseStart()`, `sseEvent($data, $event, $id, $retry)`,
  `sseComment()` and `sseRetry()` — layered on the existing streaming pipeline,
  so the same handler works over HTTP/1.1, HTTP/2 and HTTP/3. `sseStart()` sets
  the canonical headers (`Content-Type: text/event-stream`, `Cache-Control:
  no-cache, no-transform`, `X-Accel-Buffering: no`) and marks the response
  non-compressible. Framing follows WHATWG §9.2: multiline `data` is split per
  line, single-line fields reject CR/LF and `id` rejects NUL. phpt coverage for
  H1/H2/H3 plus the validation surface.

- **hq-interop (HTTP/0.9-over-QUIC) for the interop matrix (#80).** A second QUIC
  ALPN, `hq-interop`, served straight off the transport (no nghttp3): a raw bidi
  stream `GET <path>` returns the file bytes + FIN from `setHttp3HqDocroot()`.
  Lets the quic-interop-runner reach the server for the whole transport matrix
  (transfer/multiplexing/migration/loss), which it negotiates over hq, not h3.
  `h3` stays preferred when a peer offers both; the h3 path is unchanged.

- **HTTP/3 transport reactor pool (experimental, #80).** Behind
  `TRUE_ASYNC_SERVER_REACTOR_POOL=1` + `setWorkers(2+)`: dedicated C reactors own the
  QUIC sockets (no PHP on the transport thread), hand parsed requests to PHP workers
  by pointer, and serve responses back over a non-blocking reverse channel; static
  files are served on the reactor. Adds CID steering (owner-reactor id encoded in the
  connection id, forwarding migrated clients across the split — #72) and a
  migration-storm guard that sheds clients rebinding past a rate cap. Dispatch is
  reactor-paired: a connection sticks to one of its reactor's workers and spills to a
  less-loaded worker when its home backs up or dies. Off by default.
- Lock-free inter-thread message queue primitive (#81): bounded MPSC/SPSC C-ABI
  wrappers over moodycamel (`thread_queue`) plus a reactor-integrated MPSC mailbox
  (`thread_mailbox`) that wakes the consumer's loop via a trigger event with
  lost-wakeup-safe batch drain. Foundation for cross-worker HTTP/3 (#72) and
  WebSocket (#2). Adds a C++ build dependency (libstdc++).

### Fixed

- **SSE: `sseStart()` with no event now commits an empty `200` on H2/H3 (#3).**
  Starting an event stream and closing it before any `sseEvent()`/`sseComment()`
  left HTTP/2 and HTTP/3 without a HEADERS frame (the client saw a reset stream),
  while HTTP/1.1 already sent a clean empty `text/event-stream`. `mark_ended` now
  commits the empty streaming response on all three protocols.
- **SSE: mixing `send()` and the `sse*` helpers now throws (#3).** A response is
  either a plain `send()` stream or an SSE stream; crossing over silently shipped
  wrong-`Content-Type` (and possibly gzip-wrapped) event records. Each side now
  raises `HttpServerRuntimeException` once the other has committed the stream.
- **Windows: TCP listeners now bind.** The server failed to start on Windows
  with `Async\AsyncException: Failed to bind to <host>:<port>: operation not
  supported on socket`. The listener requested `SO_REUSEPORT`, which libuv's
  `uv_tcp_bind()` rejects with `UV_ENOTSUP` on Windows (Winsock has no
  `SO_REUSEPORT`). REUSEPORT is now treated as a platform capability and never
  requested on Windows; the default single-listener server binds directly. No
  change on Linux/BSD/macOS (#82).
- **Windows: `StaticHandler` accepts native absolute paths.** Root-directory
  validation only accepted a leading `/`, rejecting every Windows path
  (`C:\...`) and making `StaticHandler` unusable there. It now uses
  `IS_ABSOLUTE_PATH` (drive-letter / UNC on Windows, leading `/` on POSIX).
- **Windows: static file bodies are served binary-clean.** The `send_file`
  engine opened files without `O_BINARY`, so Windows text-mode translation
  could corrupt or truncate binary bodies (precompressed `.br`/`.gz`, byte
  ranges, images). It now opens with `O_BINARY`, matching the policy open path.

## [0.7.2] - 2026-06-02

### Added

- `HttpServerConfig::setRequestScope(bool)` / `isRequestScope()` — opt out of the
  per-request child async scope (default on). Off reuses the connection scope,
  saving two allocations per request; `Async\request_context()` then returns null
  (use `?->`). Propagates across `setWorkers(N>1)`.

## [0.7.1] - 2026-06-01

### Fixed

- HTTP/3: replenish the connection's bidi stream credit on stream close
  (`extend_max_streams_bidi`), so long-lived connections no longer stall at the
  `initial_max_streams_bidi` cap (#79).

## [0.7.0] - 2026-06-01

Headline release: **HTTP/3 over QUIC**. Folds in everything tagged but not yet
documented since 0.6.7 (the 0.6.8 tag carried no changelog entry).

### Added

- **HTTP/3 / QUIC server** (`HttpServerConfig::addHttp3Listener`) — full request
  lifecycle over QUIC: end-to-end GET/POST with `awaitBody`, streaming `send()`,
  HEAD, `sendFile()` delivery, and `addStaticHandler` mount routing. Built on
  ngtcp2 + nghttp3 + OpenSSL ≥ 3.5; auto-detected (`--enable-http3` /
  `--disable-http3`).
- HTTP/3 production controls: connection migration / NAT rebinding (RFC 9000 §9),
  opt-in send pacing (`setHttp3Pacing`), per-peer connection budget with global
  cap and explicit refusal, configurable UDP socket buffer
  (`setHttp3SocketBufferBytes`), idle timeout, Alt-Svc advertisement, Retry token
  source-address validation, version negotiation, and stateless reset.
- `HttpServer::getHttp3Stats()` — handshake / ALPN / nghttp3 / send-error counters.
- `HttpServer::isHttp2()` / `isHttp3()` compile-time capability probes.
- `HttpServerConfig::setTlsBufferBytes` — tunable TLS clear-text-out BIO ring (#29).
- Shared-fd TCP listener path for workers on kernels without load-balancing
  `SO_REUSEPORT`, selectable at runtime.

### Changed

- HTTP/3 send path coalesces outbound datagrams to once-per-tick and splits
  coalesced inbound datagrams via `UDP_GRO`; UDP socket buffers enlarged.
- HTTP/1 conformance hardening: `Date` header, HEAD sends no body, reject
  `CONNECT` and asterisk-form targets, validate `Host`, reject empty
  `Transfer-Encoding`, reject fragment/backslash in request-target, reject
  duplicate `Content-Type`.
- HTTP/2 over TLS parks the emit remainder when the clear-text-out BIO ring fills
  (backpressure instead of a write deadlock) (#29).
- `HttpServer::start()` now throws on listener bind failure instead of failing
  silently.

### Fixed

- Drain in-flight per-request coroutines on server shutdown so `server_scope` is
  not disposed while handlers are still running (#74).
- HTTP/3: dirty-list use-after-free on connection free, dispatched-stream slot
  leak when a stream is rejected mid-`awaitBody`, and `arm_timer` NULL-`ngtcp2_conn`
  guard.
- `http_server`: use-after-free of the wait event on non-stop teardown.
- Windows MSVC build.

## [0.6.7] - 2026-05-27

### Fixed

- Windows build (`config.w32`): add missing sources `src/http_response_server_api.c` and `src/http1/http1_format.c` so the MSVC build links after the `http_response.c` split landed in 0.6.6.

## [0.6.6] - 2026-05-27

### Changed

- Code audit (issue #37) — Phases 1–6 done. `src/http_response.c` (2173 lines) split into PHP-class TU + `src/http1/http1_format.c` (H/1 wire formatters) + `src/http_response_server_api.c` (server-side C-API for static/h2/compression paths). No behaviour change; phpt 211/211 PASS.

### Added

- `HttpServer::getRuntimeStats()` — snapshot of `conn_arena` (live slots, committed chunks, byte commitment) and `body_pool` (per-class LIFO of large request bodies). Pairs with `Async\runtime_stats()` and `zend_mm_dump_live_allocations()` to attribute RSS growth to a concrete subsystem.

### Fixed

- `034-config-tls-and-log.phpt`: drop the `curl_close()` call that emits a Deprecated notice on PHP 8.5+ (no-op since 8.0).
- License headers added to compression / http3 / core files that were missing them.

## [0.6.5] - 2026-05-20

### Added

- Per-request scope: each request handler coroutine now runs in its own scope, a child of the server scope, so `Async\request_context()` resolves to a context shared across the whole request coroutine subtree while `Async\current_context()` stays per-coroutine.
- IDE stubs (`ide-stubs/true-async-server.php`) for editor autocompletion of the `TrueAsync\*` API.

### Fixed

- `stubs/HttpRequest.php` was missing the `readBody()` declaration although the method ships in the extension — the stub now matches the generated arginfo.

## [0.6.4] - 2026-05-20

### Fixed

- HTTP/1 pipelining crash under high connection count (HttpArena `pipelined/4096c`): a handler-coroutine spawn failure destroyed the connection — freeing its llhttp parser — synchronously from inside `llhttp_execute` (dispatch fires from `on_headers_complete`), causing a use-after-free SIGSEGV in `on_message_complete`. Connection teardown now defers (`in_parser_feed` guard) while a parser feed is on the stack and is finalised once the feed unwinds.
- Fire-and-forget I/O write submit failures (broken pipe / connection reset) left an `Async\AsyncException` stranded in `EG(exception)` with no coroutine to receive it; it then aborted an unrelated `ZEND_ASYNC_NEW_COROUTINE` — the spawn failure above. The batched-send paths now log and clear the exception at the submission site.

## [0.6.3] - 2026-05-19

### Added

- One-shot brotli compress path with `BROTLI_PARAM_SIZE_HINT` (Step 4 of perf TODO): `apply_buffered` uses the stateless one-pass `BrotliEncoderCompress()` when the body is fully known. The size hint lets the encoder right-size its ring buffer / hash tables for the actual payload instead of for arbitrary streaming. New optional vtable slots `compress_oneshot` + `max_compressed_size`; streaming path stays for chunked / unknown-length responses. Closes the brotli encode gap vs Swoole's `BrotliEncoderCompress`-based path. C-side defaults stay production-typical (gzip 6, brotli 4); bench callers set `setCompressionLevel(1)` / `setBrotliLevel(1)` for Swoole-equivalent throughput.
- Loud stderr logging on unexpected worker thread exits in `pool_worker_handler` — covers uncaught `$server->start()` exceptions, clean returns while the await loop still expects workers, and server-transfer failure. Previously each case silently dropped 1/N of accept capacity with no operator signal.

### Fixed

- `Connection: close` request header now produces `Connection: close` in the response too (RFC 9112 §9.6). The parser already flipped `req->keep_alive = false` and the dispose path closed the FD, but the missing response header left clients unable to tell the TCP was not reusable until the next write hit ECONNRESET — wrk under `-H 'Connection: close'` counted every reply as a read error. Side effect on the local short-lived bench (wrk c=512 d=10s): 174k → 230k RPS, p50 14.5 ms → 2.5 ms, read-errors 2.0M → 0.

### Changed

- Server-side codec preference order flipped to `zstd > gzip > brotli > identity`. Clients sending the common `gzip, br` Accept-Encoding now get gzip — the brotli pool can't reuse encoder state (libbrotli has no public reset API), so until the arena-allocator follow-up (TODO Step 4) lands, gzip's `deflateReset` path is the better default. Clients that explicitly want brotli via q-values (`br;q=1.0, gzip;q=0.5`) still get it.

## [0.6.2] - 2026-05-19

### Added

- HTTP/2 over TLS hybrid emit selector (#30): small responses take the DRAIN path (mem_send + BIO_write, no gather alloc churn); bodies > 2 KiB or streaming take GATHER (NO_COPY refs + one SSL_write_ex). Streams pin a per-session counter at submit time. Bench (release PHP, c=100 m=32, h2load -t 1): dyn 3B 243k / 16K 57k / 64K 18k — hybrid best-of-three across the matrix. Env override `TRUE_ASYNC_H2_TLS_EMIT_MODE = drain | gather | hybrid` for A/B.
- `docs/H2_TLS_EMIT_STRATEGIES.md` describes the three paths and the cross-over arithmetic.

## [0.6.1] - 2026-05-18

### Fixed

- H1 handler dispatch deferred from `on_headers_complete` to `on_message_complete` for buffered bodies — a TCP-fragmented request no longer runs the handler against a partial `$req->getBody()`. Streaming handlers (`setBodyStreamingEnabled(true)`) still dispatch at headers-complete. Test: `h1/018-tcp-fragmentation.phpt`.
- Request leak in deferred-dispatch path when parse error fires between headers-complete and message-complete (chunked body cap). `parser->owns_request` is now flipped only on actual handoff.

## [0.6.0] - 2026-05-18

### Added

- `HttpServerConfig::setBootloader(?Closure)` / `getBootloader()` — closure deep-copied into each worker, runs before task loop. Requires TrueAsync ABI v0.15+. Test: `server/core/021-bootloader.phpt`.

### Fixed

- Double-destroy in `conn_arena_free` under TLS load (re-entrant destroy via `tls_finalize_if_closing` on freed conn). Guarded by `conn->destroying` bit.

### Changed

- Asymmetric TLS BIO ring sizes: CT-in 64K→17K, PT-app back-channel 32K→17K. CT-out/PT-out unchanged. Saves ~62 KiB per TLS conn, no throughput impact.

## [0.5.3] - 2026-05-16

### Fixed

- **HTTP/2 over TLS**: `Response->setBody()` with body > 64 KiB hung
  after the initial flow-control window — buffered data_provider had
  no `write_event` subscriber, WINDOW_UPDATE never reached emit. Test:
  `h2/024-h2-tls-large-body.phpt`.

- **StaticHandler open-file cache UAF**: cache stored `content_type`
  as a borrowed pointer, but the precompressed-sidecar path passed a
  transient `zend_string` (override MIME). Next cache hit dereferenced
  freed memory → heap corruption under load (HttpArena static-h2
  lite collapsed to ~190 RPS). Cache now copies. Test:
  `static/011-static-precompressed-cache-uaf.phpt`.

- **StaticHandler sync-slurp on cache hit**: the small-file shortcut
  (≤64 KiB + cache hit) bypassed the engine and dropped
  `Content-Encoding`, `Vary`, and the override `Content-Type` for
  precompressed sidecars — browsers rendered brotli as garbage. Test:
  `static/012-static-precompressed-small-cache-headers.phpt`.

### Performance

- HttpArena `static-h2` lite: 10 RPS / death-spiral → **~163k RPS**,
  0 errored (cache UAF fix).

## [0.5.2] - 2026-05-16

### Fixed

- **Windows / MSVC build**: add `src/http_body_stream.c` to `config.w32`.
  v0.5.1 only patched the CMake unit-test build; the production NMAKE
  build still failed to link with `unresolved external symbol
  http_body_stream_{push,pop,close,dispose}` from `http_parser.obj`,
  `http2_session.obj`, and `http_request.obj`.

## [0.5.1] - 2026-05-16

### Fixed

- **Windows / MSVC build**: restore the Win32 build after the streaming
  request body merge (PR #27).
  - CMake: add `src/http_body_stream.c` and the HTTP/2 sources to the
    Win32 source list; guard TLS-only sources on `OpenSSL_FOUND`.
  - Unit tests: stop letting PHP's `win32/unistd.h` / `win32/time.h`
    shadow the CRT system headers; add the four sources that
    `http_parser.c` and `multipart_processor.c` now depend on
    (`http_body_stream.c`, `core/body_pool.c`, `http_rfc5987.c`,
    `http_param_parse.c`); add a lightweight compression-vtable stub
    for `test_compression_negotiate`; prepend `PHP_DLL_DIR` and
    `CMOCKA_DLL_DIR` to PATH for every CTest target so DLL loading
    no longer fails with 0xc0000135.

Linux / macOS behaviour is unchanged — this release is Win32-only
in terms of effect.

## [0.5.0] - 2026-05-16

### Added

- `HttpRequest::readBody(int $maxLen = 65536): ?string` — pull-based
  streaming read of the request body. Returns one parser-supplied
  chunk (H2 DATA frame, default 16 KiB; H1 llhttp on_body slice,
  bounded by the H1 read buffer = 8 KiB) per call, or `null` at EOF.
  Parks the coroutine on a per-request trigger event when the queue
  is empty. Throws `\Exception` if the stream errored (peer reset,
  size cap exceeded). `$maxLen` is reserved for a future pop-side
  coalesce — kept in the signature so the eventual wiring is binary
  compatible (issue #26).
- `HttpServerConfig::setBodyStreamingEnabled(bool): static` /
  `isBodyStreamingEnabled(): bool` — server-wide flag, default
  `false`. When enabled, H1 and H2 parsers push DATA chunks into a
  per-request FIFO instead of accumulating into `req->body`, so the
  handler can consume the body via `readBody()` without ever
  materialising the full payload in memory.

### Performance

- Streaming request body (issue #26). For handlers that opt in via
  `setBodyStreamingEnabled(true)` and consume through `readBody()`,
  the per-request RSS footprint of an in-flight upload drops from
  `~Content-Length` to roughly one parser chunk. Measured on h2load
  with 50 concurrent 20 MiB POSTs (release PHP, WSL2):
  - peak RSS: 1170 MiB → **197 MiB** (~6× reduction)
  - throughput: 36 req/s → **100 req/s** (~2.7× improvement, mostly
    because handler dispatch no longer waits for the full body)
  Buffered handlers (no opt-in) keep the previous behaviour byte for
  byte; A/B benchmarks on the H1 + H2 baseline endpoints and on the
  buffered upload path show no regression beyond WSL2 measurement
  noise. Backpressure (`llhttp_pause` + deferred
  `nghttp2_session_consume`), `readBodyChunks()` for zero-copy
  scatter reads, HTTP/3 wiring, and the mode trichotomy with
  `LogicException` are tracked as follow-ups in
  `docs/PLAN_STREAMING_INGRESS.md`.

## [0.4.0] - 2026-05-11

### Performance

- Per-thread body-buffer pool for large request bodies (≥ 1 MB). Bodies
  in this size range are allocated through zend_mm but freed back to a
  thread-local LIFO instead of being released — subsequent requests of
  the same size class reuse the slot, eliminating per-request mmap /
  munmap traffic and the kernel `mmap_lock` contention that capped
  multi-worker scaling on upload-heavy workloads. Local benchmark
  (W=8, c=128, 2 MiB POST body) goes from ~1500 RPS / 370% CPU to
  ~3300 RPS / 720% CPU (×2.2 throughput; CPU now actually scales with
  workers). Drained on `HttpServer::stop()` and on PHP RSHUTDOWN; the
  debug zend_mm leak detector sees a clean slate at module unload.
  Compression decoders (gzip / brotli / zstd request body) and the
  request destructor route releases through a single `body_release()`
  helper that recognises pool-owned slots.

### Added

- `HttpResponse::sendFile(string $path, ?SendFileOptions $options = null): void`
  — handler-driven file delivery. Records path + options on the response
  and returns immediately; the protocol's `send_static_response` op
  runs the actual transfer in the dispose phase, reusing the static
  module's open-stat-sendfile FSM (MIME detection, ETag, IMF date,
  Range, conditional GET, precompressed sidecars). Path is treated as
  trusted (handler made the access decision). Open / fstat errors
  surface as a 500 since headers aren't on the wire yet.
  After `sendFile()` the response is sealed: `setHeader` / `setStatus*` /
  `write` / `send` / `setBody` / `json` / `html` / `redirect` / `end` /
  a second `sendFile()` throw `HttpServerRuntimeException`.
  New value-object `TrueAsync\SendFileOptions` (`final readonly class`,
  named-args constructor) carries `contentType`, `disposition`
  (`SendFileDisposition::INLINE | ATTACHMENT`), `downloadName`,
  `cacheControl`, `etag`, `lastModified`, `acceptRanges`,
  `precompressed`, `conditional`, `deleteAfterSend`, `status` overrides.
  Compression middleware is bypassed for sendFile bodies (own
  delivery pipeline). HTTP/3 path is follow-up — the dispose hook
  refuses with 500 for now.

### Changed

- Static-handler PHP enum cases renamed to UPPER_CASE for project-wide
  consistency: `StaticOnMissing::{NotFound→NOT_FOUND, Next→NEXT}`,
  `StaticDotfiles::{Deny→DENY, Allow→ALLOW, Ignore→IGNORE}`,
  `StaticSymlinks::{Reject→REJECT, Follow→FOLLOW, OwnerMatch→OWNER_MATCH}`.
  Breaking for any existing user code that referenced the old casings.

### Performance

- Static file delivery — inline `open(2)` / `fstat(2)` in `send_file`
  engine (issue #13). The previous `ZEND_ASYNC_FS_OPEN` /
  `ZEND_ASYNC_IO_STAT` chain routed both syscalls through the libuv
  thread pool: one futex round-trip per request just to learn whether
  a file existed. On a warm dentry cache both syscalls are sub-µs;
  the dispatch was pure overhead. A 0-ms timer defers `engine_handle_stat`
  off the synchronous tail so on_done cannot re-enter the request
  dispatcher on its own call stack. Wins: H1 tiny 256B 19k → 35k req/s,
  H1 304 If-None-Match 24k → 123k req/s.
- Static file delivery — small-file fast path (≤ 64 KiB). libuv's
  `uv_fs_sendfile` on Linux is doubly broken for file→TCP-socket: it
  tries `copy_file_range` first (returns EINVAL on socket), then falls
  into a userspace `pread` + `write` loop inside a worker thread — no
  kernel zero-copy plus a futex round-trip per request. Files at or
  under 64 KiB are now slurped synchronously into a `zend_string` and
  emitted as one `writev(headers + body)` through libuv's per-socket
  write queue; ordering with prior writes is then libuv's problem.
  Files above 64 KiB stay on the (broken-but-functional) sendfile
  path. Wins on top of the inline-syscall change: H1 tiny → 103k req/s
  (×2.9), H1 small 16K → 73k (×1.9), H2 tiny → 154k (×4.4), H2 small
  → 73k (×2.7).

### Fixed

- HTTP/2 single byte-range delivery served bytes from offset 0 of the
  file regardless of the requested range start. The H2 body FSM stored
  `body_offset` in its state but never applied it to the file: the
  buffered read path (`ZEND_ASYNC_IO_READ`) uses the fd's tracked
  position, which is 0 right after open. Seek the io once before the
  first read when `body_offset > 0`. H1 was unaffected (sendfile path
  passes the offset explicitly to the syscall).
  Closes pre-existing failure of `tests/phpt/server/static/012-static-h2`.
- Proactive drain mis-fired on the first response when CoDel/telemetry
  were disabled. The fallback timestamp used `CLOCK_MONOTONIC_COARSE`
  while `created_at_ns` and `drain_not_before_ns` are `zend_hrtime`
  (CLOCK_MONOTONIC_RAW); the two domains drift by minutes after
  suspend / NTP slewing. Drain check now stays in the same clock
  domain in both H1 dispose and H2 commit paths.

### Added

- Per-listener protocol mask (FUTURES #1). New
  `HttpServerConfig::addHttp1Listener()` / `addHttp2Listener()` for
  protocol-restricted ports (h2c-only, h1-only). Default
  `addListener()` unchanged (H1+H2).
- Built-in worker pool (issue #11). New `HttpServerConfig::setWorkers(N)`
  spawns an internal `Async\ThreadPool` from `start()`; each worker
  re-binds the listeners (`SO_REUSEPORT`). Default `1` keeps current
  behaviour bit-for-bit. Cross-thread `stop()` is a follow-up.
- `HttpResponse::json(array|string $data, int $status = 200, int $flags = 0)`
  — explicit JSON serialization through `php_json_encode_ex`. String
  passthrough for pre-encoded payloads (cached JSON ships as-is).
  Custom Content-Type set via `setHeader()` before `json()` is
  preserved (works for `application/problem+json`,
  `application/vnd.api+json`, etc.). Encode failure yields a
  controlled `500 {"error":"json encoding failed"}` — no exception
  propagation; `JSON_THROW_ON_ERROR` silently stripped. Per-server
  default flags via `HttpServerConfig::setJsonEncodeFlags()`,
  defaulting to `JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES`.
- HTTP body compression phase 2 (issue #9): Brotli (`br`) + zstd
  (`zstd`) plug into the phase-1 `http_encoder_t` vtable. Preference
  order `zstd > br > gzip > identity`; inbound `Content-Encoding: br`
  / `zstd` decoded under the same anti-bomb cap as gzip. New setters
  `setBrotliLevel(int)` (0..11, default 4), `setZstdLevel(int)`
  (1..22, default 3); `setCompressionLevel` stays gzip-only. Build
  flags `--enable-brotli` / `--enable-zstd` (auto-detect; missing libs
  warn + skip codec, build still succeeds). New introspection method
  `HttpServerConfig::getSupportedEncodings()`. Compression sources
  also wired into `config.w32` (Windows) and `CMakeLists.txt`.
  Compression coverage 80.4% → 82.9% lines.
- `docs/USAGE.md` — full configuration guide.

## [0.3.2] - 2026-05-06

### Fixed

- **Windows build broken in 0.3.0–0.3.1.** The bailout-firewall log
  line included `<sys/syscall.h>` and called `syscall(SYS_gettid)`
  unconditionally for the thread-id field — both POSIX-only, so MSVC
  failed with `fatal error C1083: Cannot open include file:
  'sys/syscall.h'`. The include is now guarded by `#ifdef _WIN32`
  (using `<windows.h>` on Windows), and the thread-id lookup uses
  `GetCurrentThreadId()` on Windows / `syscall(SYS_gettid)` elsewhere.
  Linux glibc and musl behaviour is unchanged.

## [0.3.1] - 2026-05-06

### Fixed

- **Alpine / musl libc build broken in 0.3.0.** The bailout-firewall
  diagnostic added in 0.3.0 included `<execinfo.h>` and called
  `backtrace()` / `backtrace_symbols_fd()` unconditionally — both are
  glibc-only, so musl-based images (Alpine) failed to compile with
  `fatal error: execinfo.h: No such file or directory`. The header
  is now gated behind a new `HAVE_EXECINFO_H` autoconf check
  (`AC_CHECK_HEADERS`) and the C-stack dump compiles only on
  platforms that have it. On musl / Windows the bailout fence still
  emits the PHP-level `zend_error` line via SAPI; the C-frame dump
  is silently skipped (no fake "unavailable" notice in stderr).

## [0.3.0] - 2026-05-06

### Added

- **Bailout firewall at H1/H2/H3 request boundary.** PHP fatal errors
  thrown from a user handler (E_ERROR, OOM, uncaught exceptions during
  shutdown) no longer take the server process down. Each protocol's
  request entry point now wraps the handler call in a bailout fence
  that drains the failing coroutine, emits a 500, and lets the listener
  keep accepting. Same behaviour across HTTP/1.1, HTTP/2 streams and
  HTTP/3 streams.
- **HTTP body compression** — gzip on responses and inbound request
  bodies, served identically across HTTP/1.1, HTTP/2 and HTTP/3.
  Build flag: `--enable-http-compression` (default on; auto-detects
  zlib-ng with system zlib as fallback).

  Five `HttpServerConfig` setters drive the policy and are frozen at
  `HttpServer::__construct`:
  - `setCompressionEnabled(bool)` — master switch (default `true`).
  - `setCompressionLevel(int)` — zlib level 1..9 (default 6).
  - `setCompressionMinSize(int)` — body-size threshold below which
    responses stay identity (default 1 KiB; valid 0..16 MiB).
  - `setCompressionMimeTypes(array)` — replaces the whitelist
    wholesale (nginx semantics). Default ships the union of nginx
    `gzip_types` and h2o text-only defaults.
  - `setRequestMaxDecompressedSize(int)` — anti-zip-bomb cap on
    decoded request bodies (default 10 MiB; 0 = no cap, must be
    explicit).

  Per-response opt-out: `HttpResponse::setNoCompression()` overrides
  every other rule. Use for endpoints combining secrets with
  reflected user input (BREACH mitigation), pre-encoded payloads,
  or anywhere the server must not wrap the body.

  Negotiation follows RFC 9110 §12.5.3 — q-values, `identity;q=0`,
  `*;q=0` excludes identity unless an explicit identity entry
  rescues it. Default when no `Accept-Encoding` header is sent
  resolves to identity-only (matches nginx; safer than the strict
  RFC reading). Skip rules: status 1xx/204/304, HEAD, Range
  responses, handler-set `Content-Encoding`, MIME outside the
  whitelist, body below the threshold.

  Inbound: `Content-Encoding: gzip` (and the legacy `x-gzip`
  alias) on requests is decoded transparently. `identity` is a
  no-op. Unknown codings → 415; bomb-cap exceeded → 413; corrupt
  inflate → 400. The handler observes the decoded body via
  `HttpRequest::getBody()`.

  Streaming: when handlers call `HttpResponse::send($chunk)`, the
  compressing wrapper transparently engages on first call (subject
  to negotiation) and produces one downstream chunk per source
  chunk — preserving framing efficiency on chunked H1 and H2
  DATA frames.

  Backend: `zlib-ng` is preferred at build time for ~2-4× higher
  throughput at the same compression level; system `zlib` is the
  drop-in fallback. Both share the same source via a thin
  `zng_*` ↔ `*` macro layer.

  Issue [#8](https://github.com/true-async/server/issues/8).

### Changed

- **HTTP/2 enabled by default in the build.** `--enable-http2` (Linux
  `config.m4`) and `ARG_ENABLE('http2', …)` (Windows `config.w32`) now
  default to `yes` (auto-detected via `libnghttp2 ≥ 1.57`). Previously
  the default was `no`, so a vanilla `./configure --enable-http-server`
  produced a binary whose TLS listener advertised only `http/1.1` over
  ALPN — h2 was silently absent. Use `--disable-http2` to opt out.
  Mirrors the existing HTTP/3 default.

### Fixed

- **CoDel backpressure misfired on HTTP/2 multiplexing.** The default
  CoDel queue-management hook applied per-connection sojourn estimates
  to muxed h2 streams, where short fast streams would push the
  connection into an "overloaded" state and pause unrelated long-lived
  streams. CoDel is now off by default; opt in explicitly when wanted.

## [0.2.0] - 2026-05-04

### Added

- **`HttpRequest::getPath()`** — returns the URI path without the query
  string (e.g. `/search` from `/search?q=hello`). Works identically for
  HTTP/1.1, HTTP/2 (`:path` pseudo-header), and HTTP/3.
- **`HttpRequest::getQuery(): array`** — returns all query parameters as
  an associative array, equivalent to `$_GET`. Supports percent-decoding,
  `+`-as-space, and PHP array notation (`foo[]`, `foo[bar]`).
- **`HttpRequest::getQueryParam(string $name, mixed $default = null): mixed`** —
  returns a single query parameter by name, or `$default` (null unless
  overridden) when the parameter is absent.

  All three methods share a single lazy parse: the URI is split into path
  and query string on the first call and the result is cached in the
  request struct for subsequent accesses. The query parser delegates to
  `php_default_treat_data(PARSE_STRING, …)` — the same function PHP uses
  to populate `$_GET`.

### Fixed

- **Cross-thread `HttpServer` transfer dropped requests.** When an
  `HttpServer` was passed into a worker thread (e.g. via `Async\ThreadPool`),
  `http_server_transfer_obj()` copied the registered handlers into
  `protocol_handlers` but did not mirror the corresponding
  `view.protocol_mask` bits. `detect_and_assign_protocol()` consults the mask
  to dispatch parsed requests, so worker threads bound the listen socket and
  parsed incoming bytes but silently dropped every request as if no handler
  were registered. The transfer path now sets the same mask bits that
  `addHttpHandler` / `addHttp2Handler` / `addWebSocketHandler` /
  `addGrpcHandler` set at registration time. Regression test:
  `tests/phpt/server/core/007-server-transfer-handler-dispatch.phpt`.

## [0.1.0] - 2026-04-30

Initial public release of TrueAsync Server — a native PHP extension that runs a
high-performance HTTP/1.1, HTTP/2, and HTTP/3 server directly inside PHP, built
on the [TrueAsync](https://github.com/true-async) event loop.

### Added

- **HTTP/1.1** — full RFC 9112 implementation with keep-alive and pipelining,
  built on the vendored [`llhttp`](deps/llhttp/UPSTREAM.md) 9.3.0 parser (same
  parser used by Node.js).
- **HTTP/2** — multiplexing and server push via `libnghttp2` (>= 1.57, with the
  rapid-reset mitigation floor for CVE-2023-44487).
- **HTTP/3 / QUIC** — UDP transport via `libngtcp2` + `libnghttp3`, using the
  OpenSSL 3.5 QUIC TLS API (`libngtcp2_crypto_ossl` backend). All ten ship-gates
  of the HTTP/3 plan landed: transport, TLS 1.3, request/response, streaming,
  lifecycle + drain, `Alt-Svc` advertisement, compliance smoke, and fuzzing.
- **TLS 1.2 / 1.3** — OpenSSL 3.x with ALPN negotiation, weak cipher suites
  disabled, stateless session tickets, safe renegotiation disabled.
- **Multi-protocol on a single port** — HTTP/1.1, HTTP/2, and (planned)
  WebSocket / SSE / gRPC share the same TCP listener; protocol selection via
  ALPN or HTTP `Upgrade`. HTTP/3 runs on the same UDP port and is advertised
  through `Alt-Svc`.
- **Multipart / file uploads** — streaming zero-copy parser.
- **Backpressure** — CoDel (RFC 8289) adaptive pausing on the read path.
- **Native coroutine integration** — deep integration with the TrueAsync async
  API, including the `udp_bind` hook required for HTTP/3.
- **Zero-copy architecture** — minimal allocations on hot paths.
- **Single-threaded event-loop model** — one thread owns connection and request
  end-to-end; horizontal scaling via `SO_REUSEPORT` worker processes.
- **Cross-platform builds** — Linux, macOS, and Windows (PHP-SDK / `nmake`).
  Note: HTTP/3 outbound batching uses Linux `UDP_SEGMENT` (GSO); Windows HTTP/3
  throughput is lower as a result.
- **Build system** — `config.m4` / `config.w32` / `CMakeLists.txt` with
  `--enable-http-server`, `--enable-http2`, `--enable-http3`, `--enable-tests`
  (cmocka), `--enable-coverage`, and `--without-openssl` toggles.
- **Test suite** — PHPT integration tests under `tests/phpt/` (~124 tests),
  cmocka unit tests, and a fuzzing harness under `fuzz/`.
- **CI** — extension loaded under the correct name, `run-tests.php` wired up,
  coverage baseline tracked, CodeQL analysis configuration
  (`codeql-analyze.php`).
- **Security posture** — dedicated audit covering HTTP parsing edge cases, TLS
  configuration, memory safety, and protocol-level attack vectors (HTTP request
  smuggling, HPACK bombing, QUIC amplification). Hot paths exercised under
  AddressSanitizer and Valgrind in CI.
- **Documentation** — `README.md` (overview, architecture, install for Linux
  and Windows, quick start), `docs/` (coding standards, contributor
  recommendations, llhttp upstream notes), Apache 2.0 `LICENSE`.

[Unreleased]: https://github.com/true-async/server/compare/v0.13.0...HEAD
[0.13.0]: https://github.com/true-async/server/compare/v0.12.0...v0.13.0
[0.12.0]: https://github.com/true-async/server/compare/v0.11.3...v0.12.0
[0.11.3]: https://github.com/true-async/server/compare/v0.11.2...v0.11.3
[0.11.2]: https://github.com/true-async/server/compare/v0.11.1...v0.11.2
[0.11.1]: https://github.com/true-async/server/compare/v0.11.0...v0.11.1
[0.11.0]: https://github.com/true-async/server/compare/v0.10.1...v0.11.0
[0.10.1]: https://github.com/true-async/server/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/true-async/server/compare/v0.9.3...v0.10.0
[0.9.3]: https://github.com/true-async/server/compare/v0.9.2...v0.9.3
[0.9.2]: https://github.com/true-async/server/compare/v0.9.1...v0.9.2
[0.9.1]: https://github.com/true-async/server/compare/v0.9.0...v0.9.1
[0.9.0]: https://github.com/true-async/server/compare/v0.8.1...v0.9.0
[0.8.1]: https://github.com/true-async/server/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/true-async/server/compare/v0.7.3...v0.8.0
[0.7.2]: https://github.com/true-async/server/compare/v0.7.1...v0.7.2
[0.7.1]: https://github.com/true-async/server/compare/v0.7.0...v0.7.1
[0.7.0]: https://github.com/true-async/server/compare/v0.6.7...v0.7.0
[0.6.7]: https://github.com/true-async/server/compare/v0.6.6...v0.6.7
[0.6.6]: https://github.com/true-async/server/compare/v0.6.5...v0.6.6
[0.6.5]: https://github.com/true-async/server/compare/v0.6.4...v0.6.5
[0.6.4]: https://github.com/true-async/server/compare/v0.6.3...v0.6.4
[0.6.3]: https://github.com/true-async/server/compare/v0.6.2...v0.6.3
[0.6.2]: https://github.com/true-async/server/compare/v0.6.1...v0.6.2
[0.6.1]: https://github.com/true-async/server/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/true-async/server/compare/v0.5.3...v0.6.0
[0.5.3]: https://github.com/true-async/server/compare/v0.5.2...v0.5.3
[0.5.2]: https://github.com/true-async/server/compare/v0.5.1...v0.5.2
[0.5.1]: https://github.com/true-async/server/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/true-async/server/compare/v0.4.2...v0.5.0
[0.4.0]: https://github.com/true-async/server/compare/v0.3.2...v0.4.0
[0.3.2]: https://github.com/true-async/server/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/true-async/server/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/true-async/server/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/true-async/server/compare/v0.1.5...v0.2.0
[0.1.0]: https://github.com/true-async/server/releases/tag/v0.1.0
