/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * Per-thread encoder pool — implementation.
 *
 * Storage: one LIFO per codec, sized at POOL_CAP entries. A worker
 * thread handling N concurrent compressed responses needs up to N live
 * encoders; the pool only buffers the post-release ones. Cap exists to
 * keep idle workers from holding more state than they ever needed at
 * peak (one bursty connection shouldn't pin 256 KiB × cap forever).
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "compression/http_compression_pool.h"

#include "php.h"

/* Per-codec LIFO depth. Sized so a worker handling ~hundreds of in-flight
 * compressed responses still finds a warm encoder on release. Bigger than
 * peak concurrency is wasted RSS (~256 KiB per gzip slot); smaller forces
 * destroy/create when bursts exceed the cap. 64 covers c=1000 / 16 workers. */
#define POOL_CAP 64

typedef struct {
    http_encoder_t *slots[POOL_CAP];
    int             count;
} pool_bucket_t;

ZEND_TLS pool_bucket_t pool_buckets[HTTP_CODEC__COUNT];

http_encoder_t *http_compression_pool_acquire(const http_codec_id_t id, const int level)
{
    if (UNEXPECTED(id <= HTTP_CODEC_IDENTITY || id >= HTTP_CODEC__COUNT)) {
        return NULL;
    }

    pool_bucket_t *bucket = &pool_buckets[id];

    if (bucket->count > 0) {
        http_encoder_t *enc = bucket->slots[--bucket->count];
        bucket->slots[bucket->count] = NULL;
        /* reset() is guaranteed to exist for any encoder we cached —
         * release() drops those without one. */
        if (EXPECTED(enc->vt->reset(enc, level))) {
            return enc;
        }
        /* Reset failed (rare; e.g. deflateParams hiccup). Drop and
         * fall through to a fresh create. */
        enc->vt->destroy(enc);
    }

    const http_encoder_vtable_t *vt = http_compression_lookup(id);

    if (UNEXPECTED(vt == NULL)) return NULL;
    return vt->create(level);
}

void http_compression_pool_release(http_encoder_t *enc)
{
    if (enc == NULL) return;

    const http_codec_id_t id = enc->vt->id;
    /* Codec without reset() — caching would force destroy/create on the
     * next acquire anyway; destroy immediately to free the state. */
    if (enc->vt->reset == NULL ||
        UNEXPECTED(id <= HTTP_CODEC_IDENTITY || id >= HTTP_CODEC__COUNT)) {
        enc->vt->destroy(enc);
        return;
    }

    pool_bucket_t *bucket = &pool_buckets[id];

    if (bucket->count < POOL_CAP) {
        bucket->slots[bucket->count++] = enc;
        return;
    }

    enc->vt->destroy(enc);
}

void http_compression_pool_shutdown(void)
{
    for (int id = 0; id < HTTP_CODEC__COUNT; id++) {
        pool_bucket_t *bucket = &pool_buckets[id];

        for (int j = 0; j < bucket->count; j++) {
            bucket->slots[j]->vt->destroy(bucket->slots[j]);
            bucket->slots[j] = NULL;
        }

        bucket->count = 0;
    }
}
