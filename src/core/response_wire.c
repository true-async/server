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
    void    *chunk;              /* persistent zend_string*, owned until taken */
    int      status;

    /* Growable byte arena: every span's bytes are copied in here. */
    char   *arena;
    size_t  arena_len;
    size_t  arena_cap;

    size_t  body_off, body_len;
    bool    body_set;

    wire_header_t *headers;
    size_t         header_count;
    size_t         header_cap;

    wire_header_t *trailers;
    size_t         trailer_count;
    size_t         trailer_cap;

    /* SEND_FILE payload. String spans live in the arena; *_present flags
     * distinguish an unset field from a zero-length one. */
    struct {
        size_t   path_off,  path_len;
        size_t   ct_off,    ct_len;
        size_t   dn_off,    dn_len;
        size_t   cc_off,    cc_len;
        bool     ct_present, dn_present, cc_present;
        int      status;
        uint8_t  disposition;
        bool     disposition_set, etag, last_modified, accept_ranges,
                 precompressed, conditional, delete_after_send, is_head;
    } sf;
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

void response_wire_set_chunk(response_wire_t *rw, void *persistent_str)
{
    rw->chunk = persistent_str;
}

void *response_wire_take_chunk(response_wire_t *rw)
{
    void *c = rw->chunk;
    rw->chunk = NULL;
    return c;
}

bool response_wire_set_send_file(response_wire_t *rw,
                                 const response_wire_send_file_t *sf)
{
    const size_t path_off = arena_append(rw, sf->path, sf->path_len);

    if (path_off == SIZE_MAX) {
        return false;
    }

    rw->sf.path_off = path_off;
    rw->sf.path_len = sf->path_len;

#define WIRE_SF_STR(field, off_m, len_m, present_m)                         \
    if (sf->field != NULL) {                                                \
        const size_t o = arena_append(rw, sf->field, sf->field##_len);      \
        if (o == SIZE_MAX) {                                                 \
            return false;                                                    \
        }                                                                   \
        rw->sf.off_m = o;                                                    \
        rw->sf.len_m = sf->field##_len;                                      \
        rw->sf.present_m = true;                                            \
    } else {                                                                \
        rw->sf.present_m = false;                                           \
    }

    WIRE_SF_STR(content_type,  ct_off, ct_len, ct_present)
    WIRE_SF_STR(download_name, dn_off, dn_len, dn_present)
    WIRE_SF_STR(cache_control, cc_off, cc_len, cc_present)
#undef WIRE_SF_STR

    rw->sf.status            = sf->status;
    rw->sf.disposition       = sf->disposition;
    rw->sf.disposition_set   = sf->disposition_set;
    rw->sf.etag              = sf->etag;
    rw->sf.last_modified     = sf->last_modified;
    rw->sf.accept_ranges     = sf->accept_ranges;
    rw->sf.precompressed     = sf->precompressed;
    rw->sf.conditional       = sf->conditional;
    rw->sf.delete_after_send = sf->delete_after_send;
    rw->sf.is_head           = sf->is_head;

    return true;
}

bool response_wire_get_send_file(const response_wire_t *rw,
                                 response_wire_send_file_t *out)
{
    if (rw->kind != RESPONSE_WIRE_SEND_FILE) {
        return false;
    }

    out->path              = rw->arena + rw->sf.path_off;
    out->path_len          = rw->sf.path_len;
    out->content_type      = rw->sf.ct_present ? rw->arena + rw->sf.ct_off : NULL;
    out->content_type_len  = rw->sf.ct_present ? rw->sf.ct_len : 0;
    out->download_name     = rw->sf.dn_present ? rw->arena + rw->sf.dn_off : NULL;
    out->download_name_len = rw->sf.dn_present ? rw->sf.dn_len : 0;
    out->cache_control     = rw->sf.cc_present ? rw->arena + rw->sf.cc_off : NULL;
    out->cache_control_len = rw->sf.cc_present ? rw->sf.cc_len : 0;
    out->status            = rw->sf.status;
    out->disposition       = rw->sf.disposition;
    out->disposition_set   = rw->sf.disposition_set;
    out->etag              = rw->sf.etag;
    out->last_modified     = rw->sf.last_modified;
    out->accept_ranges     = rw->sf.accept_ranges;
    out->precompressed     = rw->sf.precompressed;
    out->conditional       = rw->sf.conditional;
    out->delete_after_send = rw->sf.delete_after_send;
    out->is_head           = rw->sf.is_head;

    return true;
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

bool response_wire_set_body(response_wire_t *rw, const char *ptr, const size_t len)
{
    const size_t off = arena_append(rw, ptr, len);

    if (off == SIZE_MAX) {
        return false;
    }

    rw->body_off = off;
    rw->body_len = len;
    rw->body_set = true;

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
