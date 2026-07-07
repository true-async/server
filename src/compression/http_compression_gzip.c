/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * gzip backend — streaming deflate via zlib-ng (preferred) or zlib.
 *
 * windowBits=15+16 (bit 16 in zlib(-ng) selects the gzip wrapper:
 * 10-byte header + CRC32 trailer instead of zlib's adler32 wrap).
 * memLevel=8 is the documented default; level is the caller-provided
 * 1..9 (clamped here so nothing in the pipeline has to validate).
 *
 * Two output-side conventions worth noting:
 *   - HTTP_ENC_NEED_OUTPUT means the caller must drain `*out_produced`
 *     bytes and call back with a fresh buffer. We return it both when
 *     write() runs out of output space AND when finish() can't fit the
 *     trailer in the supplied buffer. Production callers loop until
 *     DONE; the unit test does the same.
 *   - finish() returning HTTP_ENC_OK is impossible by design — every
 *     successful exit is either NEED_OUTPUT (more flushing required)
 *     or DONE (footer emitted, stream closed).
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "compression/http_encoder.h"
#include "compression/http_compression_request.h"

#include "php.h"  /* emalloc / efree — unit tests provide a minimal Zend */
#include <string.h>

#ifdef HAVE_ZLIB_NG
#  include <zlib-ng.h>
#  define ZS                  zng_stream
#  define ZS_DEFLATE_INIT2    zng_deflateInit2
#  define ZS_DEFLATE          zng_deflate
#  define ZS_DEFLATE_END      zng_deflateEnd
#  define ZS_DEFLATE_RESET    zng_deflateReset
#  define ZS_DEFLATE_PARAMS   zng_deflateParams
#  define ZS_DEFLATE_BOUND    zng_deflateBound
#else
#  include <zlib.h>
#  define ZS                  z_stream
#  define ZS_DEFLATE_INIT2    deflateInit2
#  define ZS_DEFLATE          deflate
#  define ZS_DEFLATE_END      deflateEnd
#  define ZS_DEFLATE_RESET    deflateReset
#  define ZS_DEFLATE_PARAMS   deflateParams
#  define ZS_DEFLATE_BOUND    deflateBound
#endif

typedef struct {
    http_encoder_t base;
    ZS             stream;
    int            level;
    bool           stream_initialised;
} gzip_encoder_t;

extern const http_encoder_vtable_t http_compression_gzip_vt;

static http_encoder_t *gz_create(int level)
{
    if (level < 1) level = 1;

    if (level > 9) level = 9;

    gzip_encoder_t *enc = ecalloc(1, sizeof(*enc));
    enc->base.vt = &http_compression_gzip_vt;

    /* windowBits = MAX_WBITS (15) + 16 → gzip wrapper. */
    int rc = ZS_DEFLATE_INIT2(&enc->stream, level, Z_DEFLATED,
                              MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);

    if (rc != Z_OK) {
        efree(enc);
        return NULL;
    }

    enc->stream_initialised = true;
    enc->level              = level;
    return &enc->base;
}

static bool gz_reset(http_encoder_t *base, int level)
{
    gzip_encoder_t *enc = (gzip_encoder_t *)base;

    if (UNEXPECTED(!enc->stream_initialised)) return false;

    if (level < 1) level = 1;

    if (level > 9) level = 9;

    if (UNEXPECTED(ZS_DEFLATE_RESET(&enc->stream) != Z_OK)) return false;
    /* deflateReset preserves the level baked at init time. Adjust via
     * deflateParams only when the caller asks for a different one — the
     * common case (one level per server) avoids the param re-pack. */
    if (level != enc->level) {
        /* deflateParams may emit pending output if a stream was active;
         * we only reset()-then-write fresh streams, so avail_in/out=0
         * here is safe and the call cannot fail with Z_BUF_ERROR. */
        if (UNEXPECTED(ZS_DEFLATE_PARAMS(&enc->stream, level, Z_DEFAULT_STRATEGY) != Z_OK)) {
            return false;
        }

        enc->level = level;
    }

    return true;
}

static http_encoder_status_t gz_write(http_encoder_t *base,
                                      const void *in,  size_t in_len,  size_t *in_consumed,
                                      void       *out, size_t out_cap, size_t *out_produced)
{
    gzip_encoder_t *enc = (gzip_encoder_t *)base;

    /* zlib's API is `unsigned int avail_*` (zlib) or `size_t` (zlib-ng);
     * cast to the local type to keep both paths compiling. */
    enc->stream.next_in   = (void *)(uintptr_t)in;
    enc->stream.avail_in  = (unsigned)in_len;
    enc->stream.next_out  = (unsigned char *)out;
    enc->stream.avail_out = (unsigned)out_cap;

    int rc = ZS_DEFLATE(&enc->stream, Z_NO_FLUSH);

    if (in_consumed)  *in_consumed  = in_len  - enc->stream.avail_in;

    if (out_produced) *out_produced = out_cap - enc->stream.avail_out;

    if (rc != Z_OK) {
        return HTTP_ENC_ERROR;
    }
    /* Output buffer filled before all input was consumed → caller drains. */
    if (enc->stream.avail_out == 0 && enc->stream.avail_in > 0) {
        return HTTP_ENC_NEED_OUTPUT;
    }

    return HTTP_ENC_OK;
}

static http_encoder_status_t gz_finish(http_encoder_t *base,
                                       void *out, size_t out_cap, size_t *out_produced)
{
    gzip_encoder_t *enc = (gzip_encoder_t *)base;

    enc->stream.next_in   = NULL;
    enc->stream.avail_in  = 0;
    enc->stream.next_out  = (unsigned char *)out;
    enc->stream.avail_out = (unsigned)out_cap;

    int rc = ZS_DEFLATE(&enc->stream, Z_FINISH);

    if (out_produced) *out_produced = out_cap - enc->stream.avail_out;

    if (rc == Z_STREAM_END) return HTTP_ENC_DONE;
    /* Z_OK / Z_BUF_ERROR after Z_FINISH both mean "trailer didn't fit;
     * give me more output". */
    if (rc == Z_OK || rc == Z_BUF_ERROR) return HTTP_ENC_NEED_OUTPUT;
    return HTTP_ENC_ERROR;
}

static void gz_destroy(http_encoder_t *base)
{
    if (base == NULL) return;
    gzip_encoder_t *enc = (gzip_encoder_t *)base;

    if (enc->stream_initialised) {
        (void)ZS_DEFLATE_END(&enc->stream);
        enc->stream_initialised = false;
    }

    efree(enc);
}

const http_encoder_vtable_t http_compression_gzip_vt = {
    .name    = "gzip",
    .id      = HTTP_CODEC_GZIP,
    .create  = gz_create,
    .write   = gz_write,
    .finish  = gz_finish,
    .reset   = gz_reset,
    .destroy = gz_destroy,
};

/* One-shot whole-buffer gzip compress — see http_compression_request.h. Sizes
 * the output via deflateBound and finishes in a single deflate() pass, so it
 * never needs the streaming NEED_OUTPUT loop above. */
zend_string *http_compression_gzip_deflate_buffer(const char *in, size_t in_len,
                                                   int level)
{
    if (level < 1) level = 1;

    if (level > 9) level = 9;

    ZS s;
    memset(&s, 0, sizeof(s));

    if (ZS_DEFLATE_INIT2(&s, level, Z_DEFLATED, MAX_WBITS + 16, 8,
                         Z_DEFAULT_STRATEGY) != Z_OK) {
        return NULL;
    }

    const size_t bound = ZS_DEFLATE_BOUND(&s, in_len);
    zend_string *out   = zend_string_alloc(bound, 0);

    s.next_in   = (void *)(uintptr_t)in;
    s.avail_in  = (unsigned)in_len;
    s.next_out  = (unsigned char *)ZSTR_VAL(out);
    s.avail_out = (unsigned)bound;

    const int    rc       = ZS_DEFLATE(&s, Z_FINISH);
    const size_t produced = bound - s.avail_out;
    (void)ZS_DEFLATE_END(&s);

    if (rc != Z_STREAM_END) {
        zend_string_release(out);
        return NULL;
    }

    if (produced != bound) {
        out = zend_string_truncate(out, produced, 0);
    }

    ZSTR_VAL(out)[produced] = '\0';
    return out;
}
