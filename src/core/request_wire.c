/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Flat request representation for the reactor/worker split (#80, D2).
  See include/core/request_wire.h. Pure malloc-domain — no PHP, no ZMM.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "core/request_wire.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t name_off;
    size_t name_len;
    size_t value_off;
    size_t value_len;
} wire_header_t;

struct request_wire_s {
    uint32_t reactor_id;
    int64_t  stream_id;
    void    *conn;

    /* Growable byte arena: every span's bytes are copied in here. */
    char   *arena;
    size_t  arena_len;
    size_t  arena_cap;

    size_t  method_off, method_len;
    size_t  path_off,   path_len;
    size_t  body_off,   body_len;
    bool    method_set, path_set, body_set;
    bool    body_complete;

    wire_header_t *headers;
    size_t         header_count;
    size_t         header_cap;
};

/* Copy `len` bytes into the arena, growing it as needed. Returns the byte
 * offset of the copy, or SIZE_MAX on overflow / allocation failure. A zero
 * length appends nothing and returns the current end offset. */
static size_t arena_append(request_wire_t *rw, const char *ptr, const size_t len)
{
    if (len == 0) {
        return rw->arena_len;
    }

    if (rw->arena_len + len < rw->arena_len) {
        return SIZE_MAX; /* size_t overflow */
    }

    if (rw->arena_len + len > rw->arena_cap) {
        size_t new_cap = rw->arena_cap != 0 ? rw->arena_cap : 256;

        while (new_cap < rw->arena_len + len) {
            if (new_cap > SIZE_MAX / 2) {
                new_cap = rw->arena_len + len;
                break;
            }

            new_cap *= 2;
        }

        char *const grown = (char *) realloc(rw->arena, new_cap);

        if (grown == NULL) {
            return SIZE_MAX;
        }

        rw->arena = grown;
        rw->arena_cap = new_cap;
    }

    const size_t off = rw->arena_len;
    memcpy(rw->arena + off, ptr, len);
    rw->arena_len += len;

    return off;
}

request_wire_t *request_wire_create(const uint32_t reactor_id, const int64_t stream_id, void *conn)
{
    request_wire_t *const rw = (request_wire_t *) calloc(1, sizeof(*rw));

    if (rw == NULL) {
        return NULL;
    }

    rw->reactor_id = reactor_id;
    rw->stream_id  = stream_id;
    rw->conn       = conn;

    return rw;
}

bool request_wire_set_method(request_wire_t *rw, const char *ptr, const size_t len)
{
    const size_t off = arena_append(rw, ptr, len);

    if (off == SIZE_MAX) {
        return false;
    }

    rw->method_off = off;
    rw->method_len = len;
    rw->method_set = true;

    return true;
}

bool request_wire_set_path(request_wire_t *rw, const char *ptr, const size_t len)
{
    const size_t off = arena_append(rw, ptr, len);

    if (off == SIZE_MAX) {
        return false;
    }

    rw->path_off = off;
    rw->path_len = len;
    rw->path_set = true;

    return true;
}

bool request_wire_add_header(request_wire_t *rw,
                             const char *name_ptr, const size_t name_len,
                             const char *value_ptr, const size_t value_len)
{
    if (rw->header_count == rw->header_cap) {
        const size_t new_cap = rw->header_cap != 0 ? rw->header_cap * 2 : 8;
        wire_header_t *const grown =
            (wire_header_t *) realloc(rw->headers, new_cap * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }

        rw->headers = grown;
        rw->header_cap = new_cap;
    }

    const size_t name_off = arena_append(rw, name_ptr, name_len);

    if (name_off == SIZE_MAX) {
        return false;
    }

    const size_t value_off = arena_append(rw, value_ptr, value_len);

    if (value_off == SIZE_MAX) {
        return false;
    }

    wire_header_t *const h = &rw->headers[rw->header_count];
    h->name_off  = name_off;
    h->name_len  = name_len;
    h->value_off = value_off;
    h->value_len = value_len;
    rw->header_count++;

    return true;
}

bool request_wire_set_body(request_wire_t *rw, const char *ptr, const size_t len, const bool complete)
{
    const size_t off = arena_append(rw, ptr, len);

    if (off == SIZE_MAX) {
        return false;
    }

    rw->body_off      = off;
    rw->body_len      = len;
    rw->body_set      = true;
    rw->body_complete = complete;

    return true;
}

const char *request_wire_method(const request_wire_t *rw, size_t *len)
{
    if (!rw->method_set) {
        *len = 0;
        return NULL;
    }

    *len = rw->method_len;
    return rw->arena + rw->method_off;
}

const char *request_wire_path(const request_wire_t *rw, size_t *len)
{
    if (!rw->path_set) {
        *len = 0;
        return NULL;
    }

    *len = rw->path_len;
    return rw->arena + rw->path_off;
}

const char *request_wire_body(const request_wire_t *rw, size_t *len)
{
    if (!rw->body_set || rw->body_len == 0) {
        *len = 0;
        return rw->body_set ? rw->arena + rw->body_off : NULL;
    }

    *len = rw->body_len;
    return rw->arena + rw->body_off;
}

bool request_wire_body_complete(const request_wire_t *rw)
{
    return rw->body_complete;
}

size_t request_wire_header_count(const request_wire_t *rw)
{
    return rw->header_count;
}

bool request_wire_header_at(const request_wire_t *rw, const size_t index,
                            const char **name_ptr, size_t *name_len,
                            const char **value_ptr, size_t *value_len)
{
    if (index >= rw->header_count) {
        return false;
    }

    const wire_header_t *const h = &rw->headers[index];
    *name_ptr  = rw->arena + h->name_off;
    *name_len  = h->name_len;
    *value_ptr = rw->arena + h->value_off;
    *value_len = h->value_len;

    return true;
}

uint32_t request_wire_reactor_id(const request_wire_t *rw)
{
    return rw->reactor_id;
}

int64_t request_wire_stream_id(const request_wire_t *rw)
{
    return rw->stream_id;
}

void *request_wire_conn(const request_wire_t *rw)
{
    return rw->conn;
}

void request_wire_free(request_wire_t *rw)
{
    if (rw == NULL) {
        return;
    }

    free(rw->arena);
    free(rw->headers);
    free(rw);
}
