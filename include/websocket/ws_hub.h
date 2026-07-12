/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef WS_HUB_H
#define WS_HUB_H

#include "php.h"
#include "websocket/ws_session.h"

/*
 * Cross-worker WebSocket rooms (issue #2).
 *
 * ARCHITECTURE — share-nothing, the model uWebSockets uses for its topic tree.
 *
 * A worker is a thread that owns its sockets: only it may write to them. So a
 * room is NOT a shared list of connections. Each worker keeps its OWN member
 * table, and cross-worker work is handed to the owner as a command through its
 * mailbox (thread_mailbox.h — the #81 MPSC queue whose wakeups coalesce). A
 * ws_session_t pointer never leaves its thread; UAF is impossible by
 * construction rather than by discipline.
 *
 * A room is therefore just an identity — a name and a refcount:
 *
 *     ws_room_t { atomic refcount; zend_string *name; }
 *
 * It holds no members, no member count. Joining and leaving are purely
 * thread-local: not one atomic operation, not one lock. What crosses the thread
 * boundary is the ws_room_t POINTER (the queues carry no strings, and the
 * worker keys its local table by that pointer — no hashing on delivery). The
 * refcount is what makes the pointer safe to send: the sender takes a reference
 * per recipient, each worker drops it when done, and the room dies once the last
 * PHP object and the last in-flight command are gone. A name->id dictionary
 * would have been simpler but would never release a room — dynamic names
 * ("room:{uuid}") would leak for the life of the process.
 *
 * BROADCAST — the body is allocated once for the whole fan-out (refcounted, one
 * release per worker). The sender's own members are served inline; every OTHER
 * worker gets a command, including ones holding no members of that room: they
 * find an empty table and do nothing. Tracking who holds what would save one
 * (coalesced) wakeup and cost bookkeeping in every join, leave and teardown —
 * and getting it wrong would silently lose a message.
 *
 * COUNT — with no global tally, "how many are in the room" is a scatter/gather:
 * the caller posts a query to every other worker, each adds its local count into
 * the shared query object, and the last one to answer wakes the caller. Same
 * shape as Socket.IO's fetchSockets() across its cluster, or Phoenix's per-node
 * counters aggregated by Tracker: an eventually-consistent snapshot rather than
 * a live number — which is the honest answer here. A central counter would only
 * look exact.
 *
 * Threading:
 *   - ws_hub_create()/free() on the parent.
 *   - ws_hub_attach()/detach() on each worker (they own that thread's mailbox).
 *   - ws_hub_join/leave/leave_all() on the thread owning the session.
 *   - ws_hub_room()/room_release()/broadcast()/count() from any thread.
 */

#define WS_HUB_MAX_WORKERS 64

typedef struct ws_hub_s  ws_hub_t;
typedef struct ws_room_s ws_room_t;

ws_hub_t *ws_hub_create(void);
void      ws_hub_free(ws_hub_t *hub);

/* Claims a slot and publishes this thread's mailbox. Returns the slot, or -1. */
int  ws_hub_attach(ws_hub_t *hub);
void ws_hub_detach(void);

/* The room named `name`, created on first use. Returns it with a reference held
 * by the caller — pair with ws_room_release(). Any thread. */
ws_room_t *ws_hub_room(ws_hub_t *hub, zend_string *name);
void       ws_room_release(ws_hub_t *hub, ws_room_t *room);

zend_string *ws_room_name(const ws_room_t *room);

/* Thread-local: no locks, no atomics. */
bool ws_hub_join(ws_room_t *room, ws_session_t *session);
bool ws_hub_leave(ws_room_t *room, ws_session_t *session);
void ws_hub_leave_all(ws_session_t *session);

/* Fan out to every worker. Never suspends; a peer whose transport is backed up
 * drops the message (trySend semantics). Returns the members served on THIS
 * worker — remote delivery is asynchronous, so an exact total would be a lie. */
uint32_t ws_hub_broadcast(ws_hub_t *hub, ws_room_t *room,
                          const char *data, size_t len, bool binary,
                          uint64_t except_id);

/* Scatter/gather across workers. SUSPENDS the calling coroutine until every
 * worker has answered or `timeout_ms` elapses (a worker that misses the bound is
 * simply left out of the tally). Coroutine context only. */
uint32_t ws_hub_count(ws_hub_t *hub, ws_room_t *room, uint32_t timeout_ms);

#endif /* WS_HUB_H */
