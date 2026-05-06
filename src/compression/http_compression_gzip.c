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

#ifdef HAVE_HTTP_COMPRESSION

#include "compression/http_encoder.h"

#include "php.h"  /* emalloc / efree — unit tests provide a minimal Zend */

#ifdef HAVE_ZLIB_NG
#  include <zlib-ng.h>
#  define ZS                  zng_stream
#  define ZS_DEFLATE_INIT2    zng_deflateInit2
#  define ZS_DEFLATE          zng_deflate
#  define ZS_DEFLATE_END      zng_deflateEnd
#else
#  include <zlib.h>
#  define ZS                  z_stream
#  define ZS_DEFLATE_INIT2    deflateInit2
#  define ZS_DEFLATE          deflate
#  define ZS_DEFLATE_END      deflateEnd
#endif

typedef struct {
    http_encoder_t base;
    ZS             stream;
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
    return &enc->base;
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
    .destroy = gz_destroy,
};

#endif /* HAVE_HTTP_COMPRESSION */
