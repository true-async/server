/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * Inbound request body decoder. Phase 1: gzip via zlib(-ng).
 *
 * Anti-bomb cap is hard-required: the read loop checks decoded size
 * after every inflate() pass and aborts before realloc, so a
 * Content-Length: 1MiB body that decodes to 10 GiB never reaches the
 * limit's worth of memory on the heap.
 *
 * Output buffer growth is bounded: 4 KiB initial, doubling up to the
 * cap. Doubling avoids quadratic copy cost on large payloads while
 * staying well under what a malicious client could exploit (every
 * doubling bumps memory by 2x — cap stops us at the configured ceiling
 * regardless of how the input grows).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_http_server.h"
#include "http1/http_parser.h"
#include "compression/http_compression_request.h"
#include "core/body_pool.h"

#ifdef HAVE_ZLIB_NG
#  include <zlib-ng.h>
#  define ZS                  zng_stream
#  define ZS_INFLATE_INIT2    zng_inflateInit2
#  define ZS_INFLATE          zng_inflate
#  define ZS_INFLATE_END      zng_inflateEnd
#else
#  include <zlib.h>
#  define ZS                  z_stream
#  define ZS_INFLATE_INIT2    inflateInit2
#  define ZS_INFLATE          inflate
#  define ZS_INFLATE_END      inflateEnd
#endif

#include <string.h>

/* Inflate one gzip/zlib member; the shared inflate loop + zip-bomb guard.
 * 0 = ok (*out owned by caller), -1 = malformed, -2 = exceeds max_out. */
int http_compression_gzip_inflate_buffer(const char *in, size_t in_len,
                                         size_t max_out, zend_string **out)
{
    if (in_len == 0) {
        *out = ZSTR_EMPTY_ALLOC();
        return 0;
    }

    ZS s;
    memset(&s, 0, sizeof(s));
    /* windowBits 15+32: gzip wrapper with auto-detection (handles both
     * gzip and zlib streams gracefully — robust against clients that
     * mis-label deflate as gzip in the wild). */
    if (ZS_INFLATE_INIT2(&s, 15 + 32) != Z_OK) {
        return -1;
    }

    /* Output buffer. Initial 4 KiB, doubles on demand up to `max_out`. */
    size_t out_cap = 4096;

    if (max_out > 0 && max_out < out_cap) out_cap = max_out;
    zend_string *buf = zend_string_alloc(out_cap, 0);
    size_t produced = 0;

    s.next_in   = (void *)(uintptr_t)in;
    s.avail_in  = (unsigned)in_len;
    s.next_out  = (unsigned char *)ZSTR_VAL(buf);
    s.avail_out = (unsigned)out_cap;

    int rc;
    for (;;) {
        rc = ZS_INFLATE(&s, Z_NO_FLUSH);
        produced = out_cap - s.avail_out;

        if (rc == Z_STREAM_END) break;

        if (rc != Z_OK) {
            ZS_INFLATE_END(&s);
            zend_string_release(buf);
            return -1;
        }
        /* Need more output. Cap-aware grow: never above `max_out`. */
        if (s.avail_out == 0) {
            size_t new_cap = out_cap * 2;

            if (max_out > 0 && new_cap > max_out) {
                new_cap = max_out;
            }

            if (new_cap == out_cap) {
                /* Already at cap and inflate still wants room → bomb. */
                ZS_INFLATE_END(&s);
                zend_string_release(buf);
                return -2;
            }

            buf = zend_string_realloc(buf, new_cap, 0);
            s.next_out  = (unsigned char *)ZSTR_VAL(buf) + produced;
            s.avail_out = (unsigned)(new_cap - produced);
            out_cap = new_cap;
        }
    }

    ZS_INFLATE_END(&s);

    /* Right-size + NUL-terminate. */
    if (produced != out_cap) {
        buf = zend_string_truncate(buf, produced, 0);
    }

    ZSTR_VAL(buf)[produced] = '\0';

    *out = buf;
    return 0;
}

static int decode_gzip(http_request_t *req, size_t cap)
{
    if (req->body == NULL || ZSTR_LEN(req->body) == 0) {
        return HTTP_DECODE_OK;  /* nothing to decode */
    }

    zend_string *out = NULL;
    const int rc = http_compression_gzip_inflate_buffer(
        ZSTR_VAL(req->body), ZSTR_LEN(req->body), cap, &out);

    if (rc == -2) return HTTP_DECODE_TOO_LARGE;
    if (rc != 0)  return HTTP_DECODE_MALFORMED;

    body_release(req->body);
    req->body = out;
    req->content_length = ZSTR_LEN(out);
    return HTTP_DECODE_OK;
}

int http_compression_decode_request_body(http_request_t *req,
                                         http_server_config_t *cfg)
{
    if (req == NULL || req->headers == NULL) return HTTP_DECODE_OK;

    zval *ce = zend_hash_str_find(req->headers, "content-encoding", 16);

    if (ce == NULL || Z_TYPE_P(ce) != IS_STRING) return HTTP_DECODE_OK;

    const char *val = Z_STRVAL_P(ce);
    size_t      len = Z_STRLEN_P(ce);
    while (len > 0 && (val[0] == ' ' || val[0] == '\t')) { val++; len--; }
    while (len > 0 && (val[len - 1] == ' ' || val[len - 1] == '\t')) len--;

    if (len == 0 ||
        (len == 8 && zend_binary_strcasecmp(val, 8, "identity", 8) == 0)) {
        return HTTP_DECODE_OK;
    }

    size_t cap = (cfg != NULL) ? cfg->request_max_decompressed_size : 0;

    if (len == 4 && zend_binary_strcasecmp(val, 4, "gzip", 4) == 0) {
        return decode_gzip(req, cap);
    }
    /* Aliases — RFC 9110 lists "x-gzip" as an obsolete synonym still
     * found in older intermediaries. Decode the same way. */
    if (len == 6 && zend_binary_strcasecmp(val, 6, "x-gzip", 6) == 0) {
        return decode_gzip(req, cap);
    }
#ifdef HAVE_HTTP_BROTLI
    if (len == 2 && zend_binary_strcasecmp(val, 2, "br", 2) == 0) {
        return http_compression_decode_request_brotli(req, cap);
    }
#endif
#ifdef HAVE_HTTP_ZSTD
    if (len == 4 && zend_binary_strcasecmp(val, 4, "zstd", 4) == 0) {
        return http_compression_decode_request_zstd(req, cap);
    }
#endif

    return HTTP_DECODE_UNKNOWN_CODING;
}
