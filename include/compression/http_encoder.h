/*
 * HTTP body compression — codec-agnostic encoder/decoder vtable.
 *
 * Phase 1 ships a single backend (gzip via zlib-ng, with system zlib as
 * fallback). The vtable indirection is upfront so phase 2 codecs (Brotli,
 * zstd) plug in without touching the response pipeline. See issue #8.
 */
#ifndef HTTP_ENCODER_H
#define HTTP_ENCODER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    HTTP_CODEC_IDENTITY = 0,
    HTTP_CODEC_GZIP,
    /* HTTP_CODEC_BROTLI, HTTP_CODEC_ZSTD reserved for phase 2. */
    HTTP_CODEC__COUNT
} http_codec_id_t;

typedef enum {
    HTTP_ENC_OK = 0,         /* progress made; caller may loop */
    HTTP_ENC_NEED_OUTPUT,    /* output buffer full — drain and call again */
    HTTP_ENC_DONE,           /* finish() flushed everything */
    HTTP_ENC_ERROR
} http_encoder_status_t;

typedef struct http_encoder http_encoder_t;

typedef struct http_encoder_vtable {
    const char     *name;
    http_codec_id_t id;

    /* Allocate and initialise an encoder at the given level (1..9 for
     * gzip; backends ignore the value when not applicable). Returns NULL
     * on allocation/init failure. */
    http_encoder_t *(*create)(int level);

    /* Compress one chunk. The implementation must update *in_consumed
     * and *out_produced even when returning NEED_OUTPUT, so callers can
     * iterate on partial progress. Output buffer is caller-owned. */
    http_encoder_status_t (*write)(http_encoder_t *enc,
                                   const void *in,  size_t in_len,  size_t *in_consumed,
                                   void       *out, size_t out_cap, size_t *out_produced);

    /* Flush trailing bytes / write the codec footer. May need to be
     * called repeatedly with a refreshed output buffer until DONE. */
    http_encoder_status_t (*finish)(http_encoder_t *enc,
                                    void *out, size_t out_cap, size_t *out_produced);

    void (*destroy)(http_encoder_t *enc);
} http_encoder_vtable_t;

/* Common header. Backend-specific state follows in subclassed structs;
 * callers never touch fields beyond ->vt. */
struct http_encoder {
    const http_encoder_vtable_t *vt;
};

/* Codec registry. Returns NULL when the codec is not compiled in. */
const http_encoder_vtable_t *http_compression_lookup(http_codec_id_t id);

/* Token for Content-Encoding / Accept-Encoding ("gzip", "identity"). */
const char *http_compression_codec_token(http_codec_id_t id);

/* Build-time identifier of the gzip engine: "zlib-ng" or "zlib".
 * Used in the build banner and in diagnostic logs. */
const char *http_compression_engine_name(void);

#endif /* HTTP_ENCODER_H */
