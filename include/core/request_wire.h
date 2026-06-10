/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef REQUEST_WIRE_H
#define REQUEST_WIRE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Flat, thread-clean request representation for the reactor/worker split
 * (issue #80, design D2). The transport reactor wire-parses request headers and
 * fills a request_wire on its own thread; it is then handed to a PHP worker
 * through the #81 mailbox, and the worker materializes the `$request` zval
 * (zend_string / HashTable — per-thread ZMM) from it on ITS thread.
 *
 * Why a flat type and not http_request_t: http_request_t is built from
 * zend_string* / HashTable* (per-thread ZMM) and cannot cross threads. A
 * request_wire is pure malloc-domain bytes — no PHP allocation, no interpreter
 * needed — so the reactor (which has no usable ZMM) can produce it.
 *
 * Layout: a single owning object holds a growable byte arena plus a growable
 * header table. Every field (method, :path, header names/values, body) is
 * copied into the arena and referenced by (offset, length), so the struct is
 * self-contained and survives the recv buffer being reused — no dangling spans.
 * Accessors resolve offsets to pointers; nothing here allocates ZMM.
 *
 * Lifetime: created and filled on the reactor thread, ownership transfers to the
 * worker at post; the worker reads it (materialize) and frees it. Single owner
 * at any time — not shared, not refcounted.
 */

typedef struct request_wire_s request_wire_t;

/* Create an empty wire for one request. `conn` is an opaque reactor-owned
 * connection handle carried back on the response path; `reactor_id` / `stream_id`
 * identify the origin. Returns NULL on allocation failure. */
request_wire_t *request_wire_create(uint32_t reactor_id, int64_t stream_id, void *conn);

/* Builders — copy the bytes into the arena. Return false on allocation failure
 * (the wire stays usable/freeable). method/path replace any previous value;
 * add_header appends. All accept non-NUL-terminated spans. */
bool request_wire_set_method(request_wire_t *rw, const char *ptr, size_t len);
bool request_wire_set_path(request_wire_t *rw, const char *ptr, size_t len);
bool request_wire_add_header(request_wire_t *rw,
                             const char *name_ptr, size_t name_len,
                             const char *value_ptr, size_t value_len);
/* Set the (initial) body bytes. `complete` is false when more body will be
 * streamed to the worker separately after handoff. */
bool request_wire_set_body(request_wire_t *rw, const char *ptr, size_t len, bool complete);

/* Accessors. The returned pointers are valid until request_wire_free; *len
 * receives the span length. method/path/body return NULL with *len = 0 when
 * unset. */
const char *request_wire_method(const request_wire_t *rw, size_t *len);
const char *request_wire_path(const request_wire_t *rw, size_t *len);
const char *request_wire_body(const request_wire_t *rw, size_t *len);
bool        request_wire_body_complete(const request_wire_t *rw);

size_t request_wire_header_count(const request_wire_t *rw);
/* Resolve header `index` (0-based). Returns false for an out-of-range index. */
bool request_wire_header_at(const request_wire_t *rw, size_t index,
                            const char **name_ptr, size_t *name_len,
                            const char **value_ptr, size_t *value_len);

uint32_t request_wire_reactor_id(const request_wire_t *rw);
int64_t  request_wire_stream_id(const request_wire_t *rw);
void    *request_wire_conn(const request_wire_t *rw);

void request_wire_free(request_wire_t *rw);

#endif /* REQUEST_WIRE_H */
