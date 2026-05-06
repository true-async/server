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
 * the corresponding feature was detected. The registry below references
 * them under the same #ifdef so the linker stays happy on partial builds. */
#ifdef HAVE_HTTP_COMPRESSION
extern const http_encoder_vtable_t http_compression_gzip_vt;
#endif

const http_encoder_vtable_t *http_compression_lookup(http_codec_id_t id)
{
    switch (id) {
#ifdef HAVE_HTTP_COMPRESSION
        case HTTP_CODEC_GZIP:
            return &http_compression_gzip_vt;
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
        default:                  return NULL;
    }
}

const char *http_compression_engine_name(void)
{
#if defined(HAVE_ZLIB_NG)
    return "zlib-ng";
#elif defined(HAVE_HTTP_COMPRESSION)
    return "zlib";
#else
    return "disabled";
#endif
}
