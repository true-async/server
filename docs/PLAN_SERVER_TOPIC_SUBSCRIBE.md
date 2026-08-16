# Server-side topic subscribe — a worker receives, not only WS clients

## The gap

Topics fan a publish out to every worker, and each worker delivers it to the
**WebSocket sessions it owns** (`topic_hub.h`, `ws_topic_tree.h`). A publish can
originate anywhere — `HttpServer::publish()` / `Room::publish()` let a background
coroutine push without a socket (#137). But the **receiving** side is a
`ws_session_t` and nothing else: server-side code (a spawned run coroutine in one
worker) cannot receive a message another worker published. There is no
`subscribe` that yields to a coroutine, only `WebSocket::subscribe` that delivers
to a socket.

That blocks cross-worker coordination between workers themselves — the case that
motivated this: a run parked in worker B must be woken by an `answer`/`stop` that
arrived as an HTTP request handled in worker A. With only WS sessions as
receivers, worker B cannot hear it.

## The constraint the design must respect

The whole point of `topic_hub` is that **no `ws_session_t` pointer leaves its
thread** and there is **no shared subscriber registry** — each worker keeps its
own `ws_topic_tree` and matches the published topic string locally
(`topic_hub.h`). A server-side subscriber must obey the same rule: it is
**thread-local to the worker that created it**, lives in that worker's tree, and
is delivered to only on that worker's mailbox drain. Nothing new is shared across
threads; the publish path is unchanged (a topic string in every worker's
mailbox).

## The addition: a server subscriber

A new subscriber kind that, on a topic match, does not write a socket but
**enqueues the payload and wakes a parked coroutine** — reusing the exact async
primitive `topic_hub_count()` already uses to park/resume across a mailbox round
(`async_plain_event_new`, `zend_async_resume_when`, `ZEND_ASYNC_SUSPEND`,
`zend_async_waker_*`).

Shape of one server subscription (thread-local struct):

- a bounded ring of pending messages (text/binary + a flag);
- one `zend_async_event_t` a blocked `recv()` parks on;
- its own delivery `mark` (the once-per-pass stamp, so overlapping filters
  deliver one copy — the session path stamps the session; a server sub stamps
  itself);
- the filters it holds (for `unsubscribe_all` on close) and its interest prefix.

## Tree integration — the one real decision

`ws_topic_node_t.subs` is a dense `ws_session_t**` walked on delivery. Two ways to
admit a server subscriber:

- **(A) A parallel array** `srv_subs` at each node, walked in the same pass right
  after `subs`, with its own dead-count/tombstone and its own mark. The session
  hot path and its subtle dirty/mark/tombstone machinery are **untouched**; the
  cost is duplicating the dense-array bookkeeping for the server side.
- **(B) A unified subscriber** — `subs` becomes `ws_subscriber_t**` tagging
  session vs server, one delivery path. Less duplication, but it rewrites the
  session hot path and moves the mark off `ws_session_t`, in a file whose
  invariants are load-bearing and fuzzed.

**Recommendation: (A).** In a shared, fuzzed, carefully-tuned tree, the lowest-risk
change is the one that leaves the session path byte-for-byte as is. The duplication
is one dense array and its tombstone counter, localized to nodes; the walk gains a
second short loop. (Revisit toward (B) only if a third subscriber kind ever
appears.)

## Delivery

In the worker's drain (`topic_hub_drain` → `ws_topic_publish`), after serving
matched sessions, serve matched server subs on the same node walk, once each by
mark. Delivery to a server sub is **non-blocking, trySend semantics** — identical
contract to a socket whose transport is backed up: if the subscription's ring is
full, the message is **dropped** and counted in `topic_hub_get_stats().dropped`,
never blocking the drain. A publisher cannot be stalled by a slow server consumer,
exactly as it cannot be stalled by a slow socket.

## recv() semantics

`recv(timeoutMs?)` on the owning worker/coroutine:

- ring non-empty → pop and return immediately;
- empty → park on the event; a delivery (or `close()`) resumes it; a timeout
  resumes with `null`;
- coroutine/context rules mirror `topic_hub_count()`: outside a coroutine or in
  scheduler context there is nothing to park, so it returns immediately (empty →
  `null`).

Text vs binary: return a small message value carrying the payload and a `binary`
flag (or a text `recv()` + `recvBinary()` pair — settle in review). `close()`
unsubscribes every filter, drains/frees the ring, and wakes a parked `recv()` so
it returns `null` rather than hanging.

## PHP surface

```
$sub = $server->subscribe("control/#");     // per-worker; registers in THIS worker's tree
while (($msg = $sub->recv(30000)) !== null) { /* handle */ }
$sub->close();
```

- `HttpServer::subscribe(string $filter): TopicSubscription` — MQTT filter, same
  validator as `WebSocket::subscribe`; adds the interest prefix so publishers wake
  this worker.
- `TopicSubscription::recv(?int $timeoutMs = null): ?string` (+ binary variant /
  message object), `close(): void`. Minted only by `subscribe()`; `@not-serializable`,
  bound to its creating thread — a `recv()` from another thread throws, as
  `topic_hub_count()`'s contract already implies.

## Interest filter

A server subscription feeds `topic_hub_interest_add/remove` with its filter's
leading literal prefix, exactly like a session, so the cross-worker interest Bloom
still lets a publisher skip workers holding no match. No change to the filter
itself — a server sub is just another local subscriber contributing interest.

## Lifetime & threading invariants

- The subscription is thread-local: created, recv'd, and closed on one worker.
  Enforce the same way `count()` does — it needs `ZEND_ASYNC_CURRENT_COROUTINE`.
- On worker detach (`topic_hub_detach`), the tree is freed; any live server subs
  on it are torn down and their parked recv woken with `null`/close — same
  teardown discipline the drain already documents for sessions.
- GC: the `TopicSubscription` object owns the C struct; its destructor closes.

## Backpressure & stats

Bounded ring per subscription; overflow drops and increments the existing
`dropped` stat. No new global counter; a server consumer that falls behind looks
exactly like a slow socket, which is the honest analogy.

## Files

- `include/websocket/ws_topic_tree.h` / `src/websocket/ws_topic_tree.c` — the
  `srv_subs` array, subscribe/unsubscribe/deliver/count for server subs, mark.
- `include/websocket/topic_hub.h` / `src/websocket/topic_hub.c` — a
  `topic_hub_subscribe()` returning a handle; drain delivers to server subs.
- New `src/websocket/topic_subscription.c` (+ header) — the ring, the event, recv.
- `src/http_server_class.c` — `HttpServer::subscribe`; a `TopicSubscription`
  class, object handlers, destructor.
- `stubs/TopicSubscription.php` (+ arginfo regen), `stubs/HttpServer.php` gains
  `subscribe`.
- Tests + a fuzz target extension for the server-sub delivery path.

## Process notes (from Edmond)

- Comments on `const` / `#define` get a final cleanup pass once the code settles —
  write freely first, prune at the end.
- This plan goes to the senior-model critic before implementation.

## Open decisions

1. **(A) parallel array vs (B) unified subscriber** — recommend (A); confirm.
2. **`recv()` return** — a bare `?string` plus `recvBinary()`, or a
   `TopicMessage` value with a `binary` flag (matches `WebSocketMessage`).
3. **`subscribe()` on `HttpServer` vs on `Room`** — `HttpServer::subscribe`
   reads cleaner (a `Room` is a publish handle today); confirm the placement.
