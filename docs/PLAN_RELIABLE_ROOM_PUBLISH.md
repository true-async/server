# Reliable room delivery — the full palette

## Why

Room delivery is silently lossy: `topic_hub_publish` posts a copy into each
interested worker's bounded mailbox and, when a mailbox is full, **drops the copy
and only bumps a stat** (`src/websocket/topic_hub.c:583-589`). The sender learns
nothing. For dashboard fan-out that is fine (the next frame replaces a lost one);
for a coordination command (`answer`/`stop` to a run) it is not — a lost `stop`
hangs a run and no one knows.

The mailbox is one-directional by design (`include/core/thread_mailbox.h`): the
only wakeup is producer→consumer; there is no full→not-full signal back to a
producer. So reliability is added **above** the mailbox, on the sender's side.

## The model — NATS-style decoupling

Follow what NATS JetStream does: **do not block the sender on the slow consumer.**
Put a bounded per-worker **outbound queue** between them. A message that cannot be
posted into a target's mailbox right now is parked; a reactor-driven drainer
retries it in the background. Flow control is between the sender and the queue (its
bound), never between the sender and the slowest consumer. Best-effort `publish`
bypasses the queue entirely — it is meant to be lossy and instant.

## The palette — three calls, mirroring `WebSocket`'s trio

### `Room::publish(string $message): array` — best-effort, no queue

Post to every interested worker now; no retry, no throw. Returns the call's
breakdown — `served` local subscribers, `posted` remote mailboxes that took the
copy, `dropped` full ones that lost it, `workers` threads attached to the hub at
all. Full-mailbox drops are also in `getRuntimeStats()`.

### `Room::trySend(string $message, ?int $timeoutMs = null): bool` — non-blocking

Fan out now; for every target whose mailbox is full, park a retry entry and return
immediately: `true` if delivered outright or parked, `false` if a target was left
unserved with nothing parked for it — the queue is at `retryQueueMax`, this thread
has no queue to park on, or the message reached nobody. `false` does not mean
nothing was delivered: the fan-out ran first. The eventual outcome of a parked
message is in `getRuntimeStats()`.

### `Room::send(string $message, ?int $timeoutMs = null): int` — blocking

Same fan-out-and-park; the caller **awaits the entry** and returns targets
delivered — local subscribers plus remote mailboxes, never zero — or throws. A
send that reached nobody throws too: on this path arriving nowhere is a failure,
and "nothing is running" is a different status from "nobody has joined".

- **Semantics are at-least-once with partial delivery** (finding 3): the fast
  targets are posted during the fan-out, *before* any failure verdict. So a throw
  means "delivered to an unknown subset; a retry duplicates on the ones that
  landed." The exception carries the delivered/pending counts; for php-claw's
  coordination a duplicate is harmless because the receiver re-reads the DB
  (idempotent) — this is documented, not hidden.
- **Called outside a coroutine it THROWS** (finding 5), it does NOT degrade to
  best-effort: the one caller who chose the reliable path must not silently get
  `publish` semantics. `trySend` is the honest non-coroutine path (parking needs no
  suspension).

A convenience mirror of all three exists on `HttpServer` (no `Room` handle needed).
`WebSocket::publish()` stays best-effort only.

## Internals — the outbound queue and its drainer

Per-worker, thread-local, owned by that worker's reactor. **The queue needs no lock**
— enqueued by the worker's coroutines, drained/removed by its timer, torn down by
its detach, all one thread — so there is no lock-order question with `admin`.

### Entry ownership — the load-bearing protocol (finding 1)

Cloned from `ws_query_t` (`topic_hub.c:72-80, 683-694`), because the same
early-resume / cancellation / timeout hazards apply:

```
retry_entry = {
    zend_atomic_int refcount;          // caller holds 1 across the park; the queue holds 1
    bool            abandoned;          // set by whoever resumes/cancels first
    ws_payload_t   *payload;            // shared, refcounted (topic_hub.c:573)
    target[]        { slot, gen };      // the still-pending set
    uint64_t        deadline_ms;
    zend_async_event_t *waiter;         // non-NULL only for a blocking send()
    uint32_t        delivered, gone;    // tallied as targets resolve
}
```

- A blocking `send()` holds a ref across `ZEND_ASYNC_SUSPEND` and reads
  `delivered`/`gone` **after** resume from a struct it still owns — never from one
  the drainer may have freed.
- Whoever resumes/cancels first sets `abandoned` and clears `waiter` before
  disposing its event (exactly count()'s `abandoned = true; done = NULL` before
  `dispose`, `topic_hub.c:687-690`); the other side sees `abandoned` and skips the
  fire — no fire against a disposed event.
- The **last** `refcount` release frees the entry. `trySend` parks an entry with
  `waiter == NULL` and refcount 1 (queue only); the drainer's terminal step
  releases it.
- **Same-thread invariant (finding 10):** the drainer is a `uv_timer` on the
  sender's own loop and detach runs on that same thread, so every `waiter` fire is
  on the parked coroutine's thread — count()'s safety model (a plain in-thread
  event whose ref_count is non-atomic, `topic_hub.c:656-658`). Cooperative
  scheduling guarantees no loop turn between enqueue, `zend_async_resume_when` and
  `ZEND_ASYNC_SUSPEND`, so the first tick cannot fire before the caller is parked
  (the structural analogue of count()'s `pending > 0` guard).

### The drainer

A `uv_timer` on the worker's loop, started when the queue goes non-empty, stopped
when empty. Each tick takes `hub->admin` **once for the whole walk** (finding 7 —
the walk never suspends, so one hold per tick, not one per attempt; avoids a
lock storm of `entries × targets` acquisitions), and for each entry, each pending
target:

- `hub->inbox[slot] == NULL` **or** `gen` mismatch ⇒ the worker detached (detach
  nulls the inbox but does NOT bump `gen`, `topic_hub.c:426-428`; a matching `gen`
  is not proof of life) ⇒ resolve the target as **`retry_gone`**, not a retry
  (finding 2 — otherwise `send()` blocks the full deadline for a dead worker and
  then falsely throws);
- `thread_mailbox_post` accepts ⇒ `retry_delivered`, drop the target. **Take a
  fresh `+1` on `payload` for each posted `ws_cmd_t`** (the consumer's drain
  unconditionally releases it, `topic_hub.c:520`), mirroring publish's
  `+1`-before-post / `-1`-on-fail discipline (`topic_hub.c:581,586`) — finding 8;
- pending set empty ⇒ entry done: fire `waiter` (success), release the entry's
  payload ref once, release the entry's refcount;
- past `deadline` with a target still pending ⇒ `retry_expired`: fire `waiter`
  (→ `send()` throws), release payload, release refcount.

### Enqueue and the bound

Enqueue happens inside the fan-out: a target `thread_mailbox_post` accepts is done
at once; only full ones become the pending set; an empty pending set queues
nothing (delivered outright). Parking is **one atomic entry per message** — never a
partial park — so the cap cannot be half-hit. **The bound counts entries**
(finding 9); one entry can carry up to `TOPIC_HUB_MAX_WORKERS` `{slot,gen}` targets
(~worst-case entry weight noted so the memory ceiling is explicit). At the bound
`trySend` returns false and `send` throws, parking nothing.

## Configuration (PHP, on `HttpServerConfig`)

- `setWsPublishRetryIntervalMs(int)` — drainer cadence. Default 50.
- `setWsPublishRetryTimeoutMs(int)` — default deadline; per-call `timeoutMs`
  overrides. Default 5000.
- `setWsPublishRetryQueueMax(int)` — per-worker outbound queue cap (entries).
  Default 4096.

## Stats (added to `topic_hub_stats_t` / `getRuntimeStats()`)

`retry_queued`, `retry_delivered`, `retry_expired`, `retry_rejected` (queue full at
enqueue), `retry_gone`. Existing `dropped` stays for best-effort `publish`.

## Exception (finding 6)

A **distinct** exception for reliable-send failure — `RoomDeliveryException` (a
`WebSocketException` subclass) — NOT the existing `WebSocketBackpressureException`,
which means "rate limiter tripped, nothing sent, back off." The two must be
catch-distinguishable. It carries `delivered` and `pending` counts so a caller
knows how much landed before it decides whether re-sending (a duplicate) is safe.

## Teardown — the sharpest hazard (finding 4)

On `topic_hub_detach` (`topic_hub.c:410-453`), in order:

1. **Stop the timer** (`uv_timer_stop`) so no new tick begins.
2. **Free the timer handle only via `uv_close` + close callback** — libuv forbids
   freeing a handle synchronously; the memory is released in the close callback on
   a later loop turn (finding 4b). Detach must not `efree` the handle inline.
3. **Walk the queue**, and for each entry set `abandoned`, fire a live `waiter`
   with a shutdown error (so a parked `send()` resumes and throws rather than
   hanging), release the payload ref, release the queue's refcount.
4. **Shutdown ordering (finding 4a):** parked `send()` coroutines must be resumed
   (or cancelled) and drained *before* the queue memory is freed. Detach is the
   retire-then-drain order already used for the mailbox (`topic_hub.c:424-442`); the
   entry refcount protects against a resumed coroutine reading a freed entry, which
   is why finding 1 is a prerequisite for this section.

No new enqueue can race this: the slot is retired under `admin` first, and
cross-worker posts into a target mailbox already go through `topic_hub_post_locked`
under `admin` (so a target freeing its own mailbox mid-wait is safe — the drainer
re-reads `hub->inbox[slot]` under `admin` every attempt).

## Honest tradeoffs

- Retry latency up to `retryIntervalMs` (~50 ms), not instant — the price of a timer
  over a full→not-full wakeup the mailbox does not expose.
- `send()` blocks its caller to the deadline; `trySend` never does; `publish` never
  retries. The caller picks from the full palette.
- At-least-once with partial delivery on the reliable path (above) — a duplicate is
  possible on retry; harmless for php-claw because the receiver reconciles against
  the DB.
- Not built now (deliberate non-goal): a `Future`-returning variant. `trySend` +
  stats covers the need; a Future can be added later without breaking this.

## Files

- `include/websocket/topic_hub.h` / `src/websocket/topic_hub.c` — the outbound
  queue, the drainer timer, `topic_hub_send()` / `topic_hub_try_send()`, stats,
  detach teardown.
- `src/http_server_class.c` — `Room::send`/`trySend`, `HttpServer::send`/`trySend`,
  the three config setters; `RoomDeliveryException`.
- `stubs/*` (+ arginfo regen).
- Tests: retry-until-accept; deadline (trySend→stat, send→throw with counts);
  queue-full (trySend→false, send→throw); a detached-unreused slot resolves
  `retry_gone` (not a false expiry); gen-mismatch on a reused slot; detach with a
  non-empty queue wakes a parked `send()`, closes the timer via `uv_close`, and
  leaks nothing (ASan).
