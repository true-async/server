/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef RESPONSE_WIRE_H
#define RESPONSE_WIRE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Flat, thread-clean response representation for the reactor/worker split
 * (issue #80, design D3) — the return-path mirror of request_wire (D2). A PHP
 * worker renders its HttpResponse into a response_wire ON ITS thread (reading
 * status / headers / body out of the per-thread ZMM HttpResponse object), then
 * hands it back to the transport reactor through the #81 mailbox. The reactor
 * (no usable ZMM, owns the nghttp3/QUIC connection) QPACK-encodes the headers
 * and sends the body — it never touches a zval.
 *
 * Why a flat type and not the HttpResponse object: the response is built from
 * zend_string* / HashTable* (per-thread ZMM) and the nghttp3 encode must happen
 * on the reactor thread that owns the connection. A response_wire is pure
 * malloc-domain bytes (one growable arena, offset-based spans → realloc-safe),
 * so it crosses the thread boundary cleanly. Same layout discipline as
 * request_wire.
 *
 * Routing: reactor_id / stream_id / conn echo the originating request_wire so
 * the reactor can resolve which QUIC stream to emit on. Lifetime: created and
 * filled on the worker thread, ownership transfers to the reactor at post; the
 * reactor reads it (encode + send) and frees it. Single owner at any time.
 */

typedef struct response_wire_s response_wire_t;

/* Message kind on the reverse channel. FULL is the original single-shot shape
 * (everything in one wire at dispose). The STREAM_* kinds carry a streamed
 * response incrementally — same routing, same arena, posted in order on one
 * reactor mailbox (FIFO): one STREAM_HEADERS, any number of STREAM_CHUNKs,
 * one STREAM_END (which may carry trailers). */
typedef enum {
    RESPONSE_WIRE_FULL = 0,
    RESPONSE_WIRE_STREAM_HEADERS,
    RESPONSE_WIRE_STREAM_CHUNK,
    RESPONSE_WIRE_STREAM_END,
} response_wire_kind_t;

/* Create an empty response wire. routing identifies the origin stream the
 * reactor must send on (echoed from the request_wire). status starts unset (0).
 * Returns NULL on allocation failure. */
response_wire_t *response_wire_create(uint32_t reactor_id, int64_t stream_id, void *conn);

void                 response_wire_set_kind(response_wire_t *rw, response_wire_kind_t kind);
response_wire_kind_t response_wire_kind(const response_wire_t *rw);

/* Flow-control credit handoff (STREAM_HEADERS only): an opaque
 * stream_credit_t* riding the wire from the worker to the reactor. The wire
 * does not own it — the reactor adopts the ref at apply time (or marks it
 * dead + releases when the stream is already gone). */
void  response_wire_set_credit(response_wire_t *rw, void *credit);
void *response_wire_credit(const response_wire_t *rw);

/* Builders — copy bytes into the arena. set_status replaces; add_header
 * appends; set_body replaces. All accept non-NUL-terminated spans. The header
 * builders return false on allocation failure (the wire stays usable/freeable).
 * `complete` is false when more body will be streamed to the reactor separately
 * after this hand-off. */
void response_wire_set_status(response_wire_t *rw, int status);
bool response_wire_add_header(response_wire_t *rw,
                              const char *name_ptr, size_t name_len,
                              const char *value_ptr, size_t value_len);
bool response_wire_set_body(response_wire_t *rw, const char *ptr, size_t len, bool complete);
/* Trailers mirror headers: appended pairs, delivered by the transport after
 * the last body byte (H2 trailer HEADERS / nghttp3 submit_trailers at EOF). */
bool response_wire_add_trailer(response_wire_t *rw,
                               const char *name_ptr, size_t name_len,
                               const char *value_ptr, size_t value_len);

/* Accessors. Returned pointers are valid until response_wire_free; *len
 * receives the span length. body returns NULL with *len = 0 when unset. */
int         response_wire_status(const response_wire_t *rw);
const char *response_wire_body(const response_wire_t *rw, size_t *len);
bool        response_wire_body_complete(const response_wire_t *rw);

size_t response_wire_header_count(const response_wire_t *rw);
/* Resolve header `index` (0-based). Returns false for an out-of-range index. */
bool response_wire_header_at(const response_wire_t *rw, size_t index,
                             const char **name_ptr, size_t *name_len,
                             const char **value_ptr, size_t *value_len);

size_t response_wire_trailer_count(const response_wire_t *rw);
bool response_wire_trailer_at(const response_wire_t *rw, size_t index,
                              const char **name_ptr, size_t *name_len,
                              const char **value_ptr, size_t *value_len);

uint32_t response_wire_reactor_id(const response_wire_t *rw);
int64_t  response_wire_stream_id(const response_wire_t *rw);
void    *response_wire_conn(const response_wire_t *rw);

void response_wire_free(response_wire_t *rw);

#endif /* RESPONSE_WIRE_H */
