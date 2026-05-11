/*
 * Codec registry and module-level helpers.
 *
 * Backends declare their vtable as `extern` and this TU plugs them into
 * the registry. Lookup is the single API the response pipeline uses to
 * obtain an encoder — phase-2 codecs slot in without touching callers.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "compression/http_encoder.h"

#include <stddef.h>

/* Backend vtables. Each is provided in its own TU and only linked when
 * the corresponding feature was detected. This TU itself is excluded
 * from the build when HAVE_HTTP_COMPRESSION is undefined, so the gzip
 * vtable is always present here; brotli/zstd remain optional. */
extern const http_encoder_vtable_t http_compression_gzip_vt;
#ifdef HAVE_HTTP_BROTLI
extern const http_encoder_vtable_t http_compression_brotli_vt;
#endif
#ifdef HAVE_HTTP_ZSTD
extern const http_encoder_vtable_t http_compression_zstd_vt;
#endif

const http_encoder_vtable_t *http_compression_lookup(http_codec_id_t id)
{
    switch (id) {
        case HTTP_CODEC_GZIP:
            return &http_compression_gzip_vt;
#ifdef HAVE_HTTP_BROTLI
        case HTTP_CODEC_BROTLI:
            return &http_compression_brotli_vt;
#endif
#ifdef HAVE_HTTP_ZSTD
        case HTTP_CODEC_ZSTD:
            return &http_compression_zstd_vt;
#endif
        case HTTP_CODEC_IDENTITY:
        default:
            return NULL;
    }
}

const char *http_compression_codec_token(http_codec_id_t id)
{
    switch (id) {
        case HTTP_CODEC_IDENTITY: return "identity";
        case HTTP_CODEC_GZIP:     return "gzip";
        case HTTP_CODEC_BROTLI:   return "br";
        case HTTP_CODEC_ZSTD:     return "zstd";
        default:                  return NULL;
    }
}

const char *http_compression_engine_name(void)
{
    /* This TU only compiles when HAVE_HTTP_COMPRESSION is on; the
     * choice is between zlib-ng (preferred) and stock zlib. */
#ifdef HAVE_ZLIB_NG
    return "zlib-ng";
#else
    return "zlib";
#endif
}
