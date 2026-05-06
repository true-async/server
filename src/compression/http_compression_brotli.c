/*
 * Brotli backend — streaming encode via libbrotlienc, request-side decode
 * via libbrotlidec. Plugs into the http_encoder vtable shipped in phase 1
 * (issue #8); the response pipeline picks brotli over gzip when the client
 * advertises `br` in Accept-Encoding.
 *
 * Quality range is 0..11; we clamp here so the response pipeline does
 * not need to validate. Default level for the codec is set by the
 * config setter (HTTP_COMPRESSION_BROTLI_DEFAULT_LEVEL = 4) — at quality
 * 11 the encoder is roughly 50× slower than 4 with marginal extra ratio,
 * so 4 is the sane production default.
 *
 * The decoder mirrors the gzip request decoder: 4 KiB initial output,
 * doubling up to the configured anti-bomb cap, with the cap checked
 * before realloc so a malicious payload cannot exceed the ceiling.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_HTTP_BROTLI

#include "compression/http_encoder.h"
#include "compression/http_compression_defaults.h"
#include "compression/http_compression_request.h"

#include "php.h"
#include "php_http_server.h"
#include "http1/http_parser.h"

#include <brotli/encode.h>
#include <brotli/decode.h>

#include <stdint.h>
#include <string.h>

/* ----- encoder ------------------------------------------------------- */

typedef struct {
    http_encoder_t      base;
    BrotliEncoderState *state;
} brotli_encoder_t;

extern const http_encoder_vtable_t http_compression_brotli_vt;

static http_encoder_t *br_create(int level)
{
    if (level < HTTP_COMPRESSION_BROTLI_LEVEL_MIN) level = HTTP_COMPRESSION_BROTLI_LEVEL_MIN;
    if (level > HTTP_COMPRESSION_BROTLI_LEVEL_MAX) level = HTTP_COMPRESSION_BROTLI_LEVEL_MAX;

    BrotliEncoderState *const st = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (UNEXPECTED(st == NULL)) return NULL;
    /* BROTLI_PARAM_MODE left at default (BROTLI_MODE_GENERIC) — text-only
     * (BROTLI_MODE_TEXT) gives a ~1-2% better ratio on JSON/HTML but the
     * MIME whitelist already excludes binary; generic stays correct on
     * borderline content like SVG/XML. */
    if (UNEXPECTED(!BrotliEncoderSetParameter(st, BROTLI_PARAM_QUALITY, (uint32_t)level))) {
        BrotliEncoderDestroyInstance(st);
        return NULL;
    }

    brotli_encoder_t *const enc = ecalloc(1, sizeof(*enc));
    enc->base.vt = &http_compression_brotli_vt;
    enc->state   = st;
    return &enc->base;
}

/* Drain whatever Brotli left in its internal buffer into the caller's
 * output. Returns produced bytes; advances state internally. Used as a
 * tail step after CompressStream when the caller-supplied buffer was
 * smaller than the encoded payload. */
static size_t br_drain_output(BrotliEncoderState *const st,
                              unsigned char *const out, const size_t out_cap)
{
    size_t produced = 0;
    while (BrotliEncoderHasMoreOutput(st) && produced < out_cap) {
        size_t avail = out_cap - produced;
        const uint8_t *const p = BrotliEncoderTakeOutput(st, &avail);
        if (UNEXPECTED(p == NULL || avail == 0)) break;
        memcpy(out + produced, p, avail);
        produced += avail;
    }
    return produced;
}

static http_encoder_status_t br_write(http_encoder_t *base,
                                      const void *in,  size_t in_len,  size_t *in_consumed,
                                      void       *out, size_t out_cap, size_t *out_produced)
{
    brotli_encoder_t *const enc = (brotli_encoder_t *)base;

    /* Pass the caller buffer directly: brotli writes into it when there
     * is room, only spilling into the internal buffer if more output is
     * pending. Skips the full-buffer memcpy on the common path where one
     * caller pass absorbs the encoded chunk. */
    size_t          avail_in  = in_len;
    const uint8_t  *next_in   = (const uint8_t *)in;
    size_t          avail_out = out_cap;
    uint8_t        *next_out  = (uint8_t *)out;

    if (UNEXPECTED(!BrotliEncoderCompressStream(enc->state, BROTLI_OPERATION_PROCESS,
                                                &avail_in, &next_in,
                                                &avail_out, &next_out, NULL))) {
        if (in_consumed)  *in_consumed  = in_len  - avail_in;
        if (out_produced) *out_produced = out_cap - avail_out;
        return HTTP_ENC_ERROR;
    }

    size_t produced = out_cap - avail_out;
    /* Spilled into internal buffer — drain what fits into the remaining
     * caller space. The caller will loop on NEED_OUTPUT for the rest. */
    if (UNEXPECTED(BrotliEncoderHasMoreOutput(enc->state))) {
        produced += br_drain_output(enc->state,
            (unsigned char *)out + produced, out_cap - produced);
    }

    if (in_consumed)  *in_consumed  = in_len - avail_in;
    if (out_produced) *out_produced = produced;

    if (BrotliEncoderHasMoreOutput(enc->state)) {
        return HTTP_ENC_NEED_OUTPUT;
    }
    return HTTP_ENC_OK;
}

static http_encoder_status_t br_finish(http_encoder_t *base,
                                       void *out, size_t out_cap, size_t *out_produced)
{
    brotli_encoder_t *const enc = (brotli_encoder_t *)base;

    /* Issue FINISH only once; subsequent calls just drain. Brotli
     * tolerates re-issued FINISH after IsFinished, but skipping the
     * extra call saves a no-op trip into the encoder. */
    if (!BrotliEncoderIsFinished(enc->state)) {
        size_t          avail_in  = 0;
        const uint8_t  *next_in   = NULL;
        size_t          avail_out = out_cap;
        uint8_t        *next_out  = (uint8_t *)out;
        if (UNEXPECTED(!BrotliEncoderCompressStream(enc->state, BROTLI_OPERATION_FINISH,
                                                    &avail_in, &next_in,
                                                    &avail_out, &next_out, NULL))) {
            if (out_produced) *out_produced = out_cap - avail_out;
            return HTTP_ENC_ERROR;
        }
        size_t produced = out_cap - avail_out;
        if (UNEXPECTED(BrotliEncoderHasMoreOutput(enc->state))) {
            produced += br_drain_output(enc->state,
                (unsigned char *)out + produced, out_cap - produced);
        }
        if (out_produced) *out_produced = produced;
    } else {
        /* Already finished; just drain whatever is still buffered. */
        const size_t produced = br_drain_output(enc->state, (unsigned char *)out, out_cap);
        if (out_produced) *out_produced = produced;
    }

    if (BrotliEncoderHasMoreOutput(enc->state)) {
        return HTTP_ENC_NEED_OUTPUT;
    }
    if (EXPECTED(BrotliEncoderIsFinished(enc->state))) {
        return HTTP_ENC_DONE;
    }
    /* FINISH issued, no buffered output, but encoder reports unfinished —
     * shouldn't happen in practice; ask for another pass. */
    return HTTP_ENC_NEED_OUTPUT;
}

static void br_destroy(http_encoder_t *base)
{
    if (base == NULL) return;
    brotli_encoder_t *enc = (brotli_encoder_t *)base;
    if (enc->state) {
        BrotliEncoderDestroyInstance(enc->state);
        enc->state = NULL;
    }
    efree(enc);
}

const http_encoder_vtable_t http_compression_brotli_vt = {
    .name    = "br",
    .id      = HTTP_CODEC_BROTLI,
    .create  = br_create,
    .write   = br_write,
    .finish  = br_finish,
    .destroy = br_destroy,
};

/* ----- request decoder ----------------------------------------------- */

int http_compression_decode_request_brotli(http_request_t *req, size_t cap)
{
    if (req->body == NULL || ZSTR_LEN(req->body) == 0) {
        return HTTP_DECODE_OK;
    }

    BrotliDecoderState *const st = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (UNEXPECTED(st == NULL)) {
        return HTTP_DECODE_MALFORMED;
    }

    /* Output buffer mirrors the gzip path: 4 KiB initial, doubling
     * (capped) until the decoder reports DONE or the cap is hit. */
    size_t out_cap = 4096;
    if (cap > 0 && cap < out_cap) out_cap = cap;
    zend_string *out = zend_string_alloc(out_cap, 0);
    size_t produced = 0;

    const uint8_t *next_in  = (const uint8_t *)ZSTR_VAL(req->body);
    size_t         avail_in = ZSTR_LEN(req->body);
    uint8_t       *next_out = (uint8_t *)ZSTR_VAL(out);
    size_t         avail_out = out_cap;

    for (;;) {
        const BrotliDecoderResult r = BrotliDecoderDecompressStream(
            st, &avail_in, &next_in, &avail_out, &next_out, NULL);
        produced = out_cap - avail_out;

        if (EXPECTED(r == BROTLI_DECODER_RESULT_SUCCESS)) break;
        if (UNEXPECTED(r == BROTLI_DECODER_RESULT_ERROR ||
                       r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT)) {
            /* NEEDS_MORE_INPUT after we already passed the entire body
             * means the stream is truncated — treat as malformed. */
            BrotliDecoderDestroyInstance(st);
            zend_string_release(out);
            return HTTP_DECODE_MALFORMED;
        }
        /* BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT — grow within cap. */
        size_t new_cap = out_cap * 2;
        if (cap > 0 && new_cap > cap) new_cap = cap;
        if (UNEXPECTED(new_cap == out_cap)) {
            BrotliDecoderDestroyInstance(st);
            zend_string_release(out);
            return HTTP_DECODE_TOO_LARGE;
        }
        out       = zend_string_realloc(out, new_cap, 0);
        next_out  = (uint8_t *)ZSTR_VAL(out) + produced;
        avail_out = new_cap - produced;
        out_cap   = new_cap;
    }
    BrotliDecoderDestroyInstance(st);

    if (produced != out_cap) {
        out = zend_string_truncate(out, produced, 0);
    }
    ZSTR_VAL(out)[produced] = '\0';

    zend_string_release(req->body);
    req->body = out;
    req->content_length = produced;
    return HTTP_DECODE_OK;
}

#endif /* HAVE_HTTP_BROTLI */
