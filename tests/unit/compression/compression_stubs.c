/*
 * Stubs for compression backend vtables used in negotiate unit tests.
 *
 * The negotiate test exercises Accept-Encoding parsing + codec selection
 * logic only. It does not perform actual compression, so we avoid linking
 * the real gzip/brotli/zstd backends (and their external library deps).
 * http_compression_lookup returns a pointer to a zero-initialized static
 * vtable for every known codec so the selection logic sees all codecs as
 * "registered" — matching a build that has all three backends enabled.
 */

#include "compression/http_encoder.h"

static const http_encoder_vtable_t stub_vt = {0};

const http_encoder_vtable_t *http_compression_lookup(http_codec_id_t id)
{
    switch (id) {
        case HTTP_CODEC_GZIP:
        case HTTP_CODEC_BROTLI:
        case HTTP_CODEC_ZSTD:
            return &stub_vt;
        default:
            return NULL;
    }
}
