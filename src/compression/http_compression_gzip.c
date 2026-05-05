/*
 * gzip backend — skeleton.
 *
 * Phase-1 commit 1 wires the vtable so the response pipeline can be
 * built against a stable symbol. Streaming compression (deflate via
 * zlib-ng with window=15+16 for the gzip wrapper) lands in commit 4.
 *
 * The stub create() returns NULL so any accidental call site fails
 * loudly instead of silently emitting garbage.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_HTTP_COMPRESSION

#include "compression/http_encoder.h"

#include <stddef.h>

static http_encoder_t *gz_create(int level)
{
    (void)level;
    return NULL; /* TODO(#8 commit 4): zng_deflateInit2 / deflateInit2 */
}

static http_encoder_status_t gz_write(http_encoder_t *enc,
                                      const void *in,  size_t in_len,  size_t *in_consumed,
                                      void       *out, size_t out_cap, size_t *out_produced)
{
    (void)enc; (void)in; (void)in_len; (void)out; (void)out_cap;
    if (in_consumed)  *in_consumed  = 0;
    if (out_produced) *out_produced = 0;
    return HTTP_ENC_ERROR;
}

static http_encoder_status_t gz_finish(http_encoder_t *enc,
                                       void *out, size_t out_cap, size_t *out_produced)
{
    (void)enc; (void)out; (void)out_cap;
    if (out_produced) *out_produced = 0;
    return HTTP_ENC_ERROR;
}

static void gz_destroy(http_encoder_t *enc)
{
    (void)enc;
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
