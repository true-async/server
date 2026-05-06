/*
 * zstd backend — streaming encode + decode via libzstd. Plugs into the
 * http_encoder vtable shipped in phase 1; same shape as the brotli TU
 * but a single library covers both directions.
 *
 * Compression level range is 1..22; level 3 is the zstd team's own
 * production default (~gzip-6 ratio at ~3× the throughput). Negative
 * "fast" levels are not exposed — they trade ratio for speed below
 * gzip-1 territory and are rarely useful on HTTP payloads.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_HTTP_ZSTD

#include "compression/http_encoder.h"
#include "compression/http_compression_defaults.h"
#include "compression/http_compression_request.h"

#include "php.h"
#include "php_http_server.h"
#include "http1/http_parser.h"

#include <zstd.h>

#include <stdint.h>
#include <string.h>

/* ----- encoder ------------------------------------------------------- */

typedef struct {
    http_encoder_t base;
    ZSTD_CStream  *cstream;
} zstd_encoder_t;

extern const http_encoder_vtable_t http_compression_zstd_vt;

static http_encoder_t *zs_create(int level)
{
    if (level < HTTP_COMPRESSION_ZSTD_LEVEL_MIN) level = HTTP_COMPRESSION_ZSTD_LEVEL_MIN;
    if (level > HTTP_COMPRESSION_ZSTD_LEVEL_MAX) level = HTTP_COMPRESSION_ZSTD_LEVEL_MAX;
    /* Belt-and-braces against future level-cap changes in libzstd. */
    const int max_cl = ZSTD_maxCLevel();
    if (level > max_cl) level = max_cl;

    ZSTD_CStream *const cs = ZSTD_createCStream();
    if (UNEXPECTED(cs == NULL)) return NULL;

    /* ZSTD_CCtx_setParameter is the modern way; ZSTD_initCStream is
     * deprecated (it bakes pledgedSrcSize=0 and a fixed level). */
    const size_t rc = ZSTD_CCtx_setParameter(cs, ZSTD_c_compressionLevel, level);
    if (UNEXPECTED(ZSTD_isError(rc))) {
        ZSTD_freeCStream(cs);
        return NULL;
    }

    zstd_encoder_t *const enc = ecalloc(1, sizeof(*enc));
    enc->base.vt = &http_compression_zstd_vt;
    enc->cstream = cs;
    return &enc->base;
}

static http_encoder_status_t zs_write(http_encoder_t *base,
                                      const void *in,  size_t in_len,  size_t *in_consumed,
                                      void       *out, size_t out_cap, size_t *out_produced)
{
    zstd_encoder_t *const enc = (zstd_encoder_t *)base;

    ZSTD_inBuffer  ibuf = { .src = in,  .size = in_len,  .pos = 0 };
    ZSTD_outBuffer obuf = { .dst = out, .size = out_cap, .pos = 0 };

    /* compressStream2(continue) writes directly into the caller buffer;
     * pos fields tell us how much of each side moved. Returns a hint
     * (>0) on success, 0 when nothing is buffered, or an ZSTD_isError
     * value on failure. */
    const size_t rc = ZSTD_compressStream2(enc->cstream, &obuf, &ibuf, ZSTD_e_continue);

    if (in_consumed)  *in_consumed  = ibuf.pos;
    if (out_produced) *out_produced = obuf.pos;

    if (UNEXPECTED(ZSTD_isError(rc))) return HTTP_ENC_ERROR;

    /* Output filled before all input was consumed → caller drains. */
    if (obuf.pos == obuf.size && ibuf.pos < ibuf.size) {
        return HTTP_ENC_NEED_OUTPUT;
    }
    return HTTP_ENC_OK;
}

static http_encoder_status_t zs_finish(http_encoder_t *base,
                                       void *out, size_t out_cap, size_t *out_produced)
{
    zstd_encoder_t *const enc = (zstd_encoder_t *)base;

    ZSTD_inBuffer  ibuf = { .src = NULL, .size = 0, .pos = 0 };
    ZSTD_outBuffer obuf = { .dst = out,  .size = out_cap, .pos = 0 };

    /* ZSTD_e_end flushes the current block and writes the frame
     * epilogue. Return value is the residual bytes still buffered;
     * 0 means the frame is fully written. */
    const size_t remaining = ZSTD_compressStream2(enc->cstream, &obuf, &ibuf, ZSTD_e_end);
    if (out_produced) *out_produced = obuf.pos;

    if (UNEXPECTED(ZSTD_isError(remaining))) return HTTP_ENC_ERROR;
    if (EXPECTED(remaining == 0))            return HTTP_ENC_DONE;
    return HTTP_ENC_NEED_OUTPUT;
}

static void zs_destroy(http_encoder_t *base)
{
    if (base == NULL) return;
    zstd_encoder_t *enc = (zstd_encoder_t *)base;
    if (enc->cstream) {
        ZSTD_freeCStream(enc->cstream);
        enc->cstream = NULL;
    }
    efree(enc);
}

const http_encoder_vtable_t http_compression_zstd_vt = {
    .name    = "zstd",
    .id      = HTTP_CODEC_ZSTD,
    .create  = zs_create,
    .write   = zs_write,
    .finish  = zs_finish,
    .destroy = zs_destroy,
};

/* ----- request decoder ----------------------------------------------- */

int http_compression_decode_request_zstd(http_request_t *req, size_t cap)
{
    if (req->body == NULL || ZSTR_LEN(req->body) == 0) {
        return HTTP_DECODE_OK;
    }

    ZSTD_DStream *const ds = ZSTD_createDStream();
    if (UNEXPECTED(ds == NULL)) return HTTP_DECODE_MALFORMED;

    /* Same growth schedule as the gzip/brotli decoders for parity. */
    size_t out_cap = 4096;
    if (cap > 0 && cap < out_cap) out_cap = cap;
    zend_string *out = zend_string_alloc(out_cap, 0);
    size_t produced = 0;

    ZSTD_inBuffer ibuf = {
        .src  = ZSTR_VAL(req->body),
        .size = ZSTR_LEN(req->body),
        .pos  = 0,
    };

    for (;;) {
        ZSTD_outBuffer obuf = {
            .dst  = ZSTR_VAL(out) + produced,
            .size = out_cap - produced,
            .pos  = 0,
        };
        const size_t rc = ZSTD_decompressStream(ds, &obuf, &ibuf);
        produced += obuf.pos;

        if (UNEXPECTED(ZSTD_isError(rc))) {
            ZSTD_freeDStream(ds);
            zend_string_release(out);
            return HTTP_DECODE_MALFORMED;
        }
        if (EXPECTED(rc == 0)) break;   /* frame fully decoded */

        /* rc > 0 → decoder needs more input or output. If the input
         * is exhausted but the frame isn't done, the body was truncated. */
        if (UNEXPECTED(ibuf.pos == ibuf.size && obuf.pos < obuf.size)) {
            ZSTD_freeDStream(ds);
            zend_string_release(out);
            return HTTP_DECODE_MALFORMED;
        }
        /* Need more output room — grow within cap. */
        if (produced == out_cap) {
            size_t new_cap = out_cap * 2;
            if (cap > 0 && new_cap > cap) new_cap = cap;
            if (UNEXPECTED(new_cap == out_cap)) {
                ZSTD_freeDStream(ds);
                zend_string_release(out);
                return HTTP_DECODE_TOO_LARGE;
            }
            out     = zend_string_realloc(out, new_cap, 0);
            out_cap = new_cap;
        }
    }
    ZSTD_freeDStream(ds);

    if (produced != out_cap) {
        out = zend_string_truncate(out, produced, 0);
    }
    ZSTR_VAL(out)[produced] = '\0';

    zend_string_release(req->body);
    req->body = out;
    req->content_length = produced;
    return HTTP_DECODE_OK;
}

#endif /* HAVE_HTTP_ZSTD */
