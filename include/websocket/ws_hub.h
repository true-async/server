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
 * Cross-worker WebSocket rooms (issue #2). Share-nothing, the model uWebSockets
 * uses for its topic tree.
 *
 * Only the owning thread may write to a socket, so a room is NOT a shared list
 * of connections: each worker keeps its OWN member table, and cross-worker work
 * is handed to the owner through its mailbox (thread_mailbox.h). A ws_session_t
 * pointer never leaves its thread — UAF is impossible by construction rather
 * than by discipline.
 *
 * A room is therefore just an identity: a name and an atomic refcount. It holds
 * no members and no member count, so join/leave are purely thread-local — not
 * one atomic, not one lock. What crosses the boundary is the ws_room_t POINTER,
 * and the refcount is what makes that safe to send: sender takes one per
 * recipient, each worker drops its own. It also lets a room die once its last
 * member and last in-flight command are gone; a name->id dictionary would never
 * release one, so dynamic names ("room:{uuid}") would leak for the process's
 * life.
 *
 * Broadcast goes to EVERY worker, including ones holding no members of the room:
 * they find an empty table and do nothing. Tracking who holds what would save
 * one (coalesced) wakeup and cost bookkeeping in every join, leave and teardown
 * — and getting it wrong would silently lose a message.
 *
 * Threading:
 *   - create()/addref()/release() from any thread.
 *   - attach()/detach() on each worker (they own that thread's mailbox).
 *   - join/leave/leave_all() on the thread owning the session.
 *   - room()/room_release()/broadcast()/count() from any thread.
 */

#define WS_HUB_MAX_WORKERS 64

typedef struct ws_hub_s  ws_hub_t;
typedef struct ws_room_s ws_room_t;

/* Refcounted: a PHP WebSocketRoom can outlive the server that minted it (it is
 * an ordinary object the script may still hold when start() returns), and every
 * room drop needs the hub's mutex. */
ws_hub_t *ws_hub_create(void);
void      ws_hub_addref(ws_hub_t *hub);
void      ws_hub_release(ws_hub_t *hub);

/* Claims a slot and publishes this thread's mailbox. Returns the slot, or -1. */
int  ws_hub_attach(ws_hub_t *hub);
void ws_hub_detach(void);

/* The room named `name`, created on first use. Returns a reference the caller
 * owns — pair with ws_room_release(). Any thread. */
ws_room_t *ws_hub_room(ws_hub_t *hub, zend_string *name);
void       ws_room_release(ws_hub_t *hub, ws_room_t *room);

zend_string *ws_room_name(const ws_room_t *room);

/* Thread-local: no locks, no atomics. */
bool ws_hub_join(ws_room_t *room, ws_session_t *session);
bool ws_hub_leave(ws_room_t *room, ws_session_t *session);
void ws_hub_leave_all(ws_session_t *session);

/* Never suspends; a peer whose transport is backed up drops the message (trySend
 * semantics). Returns the members served on THIS worker — remote delivery is
 * asynchronous, so an exact total would be a lie.
 *
 * A remote worker whose mailbox is full also drops the message, and that one is
 * NOT visible in the return value: it is counted in ws_hub_dropped() instead. */
uint32_t ws_hub_broadcast(ws_hub_t *hub, ws_room_t *room,
                          const char *data, size_t len, bool binary,
                          uint64_t except_id);

/* Scatter/gather: no global tally exists, so each worker answers with its own
 * count (same shape as Socket.IO's fetchSockets, or Phoenix's per-node counters).
 * SUSPENDS the caller; a worker that misses `timeout_ms` is left out of the sum,
 * so the result is a snapshot, not a live number. Coroutine context only. */
uint32_t ws_hub_count(ws_hub_t *hub, ws_room_t *room, uint32_t timeout_ms);

/* Messages this hub could not hand to a worker because its mailbox was full,
 * process-wide since start. A rising count means a worker is not draining fast
 * enough (or a client is flooding broadcasts) and room traffic is being lost. */
uint64_t ws_hub_dropped(ws_hub_t *hub);

#endif /* WS_HUB_H */
