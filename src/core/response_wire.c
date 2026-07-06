/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Flat response representation for the reactor/worker split (#80, D3).
  See include/core/response_wire.h. Pure malloc-domain — no PHP, no ZMM.
  The return-path mirror of request_wire.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "core/response_wire.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t name_off;
    size_t name_len;
    size_t value_off;
    size_t value_len;
} wire_header_t;

struct response_wire_s {
    uint32_t reactor_id;
    int64_t  stream_id;
    void    *conn;

    response_wire_kind_t kind;   /* FULL unless set otherwise */
    void    *credit;             /* opaque stream_credit_t*, not owned */
    int      status;

    /* Growable byte arena: every span's bytes are copied in here. */
    char   *arena;
    size_t  arena_len;
    size_t  arena_cap;

    size_t  body_off, body_len;
    bool    body_set;
    bool    body_complete;

    wire_header_t *headers;
    size_t         header_count;
    size_t         header_cap;

    wire_header_t *trailers;
    size_t         trailer_count;
    size_t         trailer_cap;
};

/* Copy `len` bytes into the arena, growing it as needed. Returns the byte
 * offset of the copy, or SIZE_MAX on overflow / allocation failure. A zero
 * length appends nothing and returns the current end offset. */
static size_t arena_append(response_wire_t *rw, const char *ptr, const size_t len)
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

response_wire_t *response_wire_create(const uint32_t reactor_id, const int64_t stream_id, void *conn)
{
    response_wire_t *const rw = (response_wire_t *) calloc(1, sizeof(*rw));

    if (rw == NULL) {
        return NULL;
    }

    rw->reactor_id = reactor_id;
    rw->stream_id  = stream_id;
    rw->conn       = conn;

    return rw;
}

void response_wire_set_kind(response_wire_t *rw, const response_wire_kind_t kind)
{
    rw->kind = kind;
}

response_wire_kind_t response_wire_kind(const response_wire_t *rw)
{
    return rw->kind;
}

void response_wire_set_credit(response_wire_t *rw, void *credit)
{
    rw->credit = credit;
}

void *response_wire_credit(const response_wire_t *rw)
{
    return rw->credit;
}

void response_wire_set_status(response_wire_t *rw, const int status)
{
    rw->status = status;
}

/* Append one (name, value) pair to a wire_header_t list (headers or trailers),
 * copying both spans into the arena. */
static bool wire_pair_add(response_wire_t *rw,
                          wire_header_t **list, size_t *count, size_t *cap,
                          const char *name_ptr, const size_t name_len,
                          const char *value_ptr, const size_t value_len)
{
    if (*count == *cap) {
        const size_t new_cap = *cap != 0 ? *cap * 2 : 8;
        wire_header_t *const grown =
            (wire_header_t *) realloc(*list, new_cap * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }

        *list = grown;
        *cap  = new_cap;
    }

    const size_t name_off = arena_append(rw, name_ptr, name_len);

    if (name_off == SIZE_MAX) {
        return false;
    }

    const size_t value_off = arena_append(rw, value_ptr, value_len);

    if (value_off == SIZE_MAX) {
        return false;
    }

    wire_header_t *const h = &(*list)[*count];
    h->name_off  = name_off;
    h->name_len  = name_len;
    h->value_off = value_off;
    h->value_len = value_len;
    (*count)++;

    return true;
}

bool response_wire_add_header(response_wire_t *rw,
                              const char *name_ptr, const size_t name_len,
                              const char *value_ptr, const size_t value_len)
{
    return wire_pair_add(rw, &rw->headers, &rw->header_count, &rw->header_cap,
                         name_ptr, name_len, value_ptr, value_len);
}

bool response_wire_add_trailer(response_wire_t *rw,
                               const char *name_ptr, const size_t name_len,
                               const char *value_ptr, const size_t value_len)
{
    return wire_pair_add(rw, &rw->trailers, &rw->trailer_count, &rw->trailer_cap,
                         name_ptr, name_len, value_ptr, value_len);
}

bool response_wire_set_body(response_wire_t *rw, const char *ptr, const size_t len, const bool complete)
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

int response_wire_status(const response_wire_t *rw)
{
    return rw->status;
}

const char *response_wire_body(const response_wire_t *rw, size_t *len)
{
    if (!rw->body_set || rw->body_len == 0) {
        *len = 0;
        return rw->body_set ? rw->arena + rw->body_off : NULL;
    }

    *len = rw->body_len;
    return rw->arena + rw->body_off;
}

bool response_wire_body_complete(const response_wire_t *rw)
{
    return rw->body_complete;
}

size_t response_wire_header_count(const response_wire_t *rw)
{
    return rw->header_count;
}

/* Resolve entry `index` of a wire_header_t list against the arena. */
static bool wire_pair_at(const response_wire_t *rw,
                         const wire_header_t *list, const size_t count,
                         const size_t index,
                         const char **name_ptr, size_t *name_len,
                         const char **value_ptr, size_t *value_len)
{
    if (index >= count) {
        return false;
    }

    const wire_header_t *const h = &list[index];
    *name_ptr  = rw->arena + h->name_off;
    *name_len  = h->name_len;
    *value_ptr = rw->arena + h->value_off;
    *value_len = h->value_len;

    return true;
}

bool response_wire_header_at(const response_wire_t *rw, const size_t index,
                             const char **name_ptr, size_t *name_len,
                             const char **value_ptr, size_t *value_len)
{
    return wire_pair_at(rw, rw->headers, rw->header_count, index,
                        name_ptr, name_len, value_ptr, value_len);
}

size_t response_wire_trailer_count(const response_wire_t *rw)
{
    return rw->trailer_count;
}

bool response_wire_trailer_at(const response_wire_t *rw, const size_t index,
                              const char **name_ptr, size_t *name_len,
                              const char **value_ptr, size_t *value_len)
{
    return wire_pair_at(rw, rw->trailers, rw->trailer_count, index,
                        name_ptr, name_len, value_ptr, value_len);
}

uint32_t response_wire_reactor_id(const response_wire_t *rw)
{
    return rw->reactor_id;
}

int64_t response_wire_stream_id(const response_wire_t *rw)
{
    return rw->stream_id;
}

void *response_wire_conn(const response_wire_t *rw)
{
    return rw->conn;
}

void response_wire_free(response_wire_t *rw)
{
    if (rw == NULL) {
        return;
    }

    free(rw->arena);
    free(rw->headers);
    free(rw->trailers);
    free(rw);
}
