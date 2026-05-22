/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * Per-thread encoder pool.
 *
 * deflateInit2/BrotliEncoderCreateInstance/ZSTD_createCStream each allocate
 * sizable internal state (zlib alone is ~256 KiB at level 6). Recreating
 * one per response caps throughput around 25-30k RPS per core on small
 * bodies. The pool keeps a per-thread LIFO of warm encoders and hands
 * them out via reset() when the codec exposes that API.
 */
#ifndef HTTP_COMPRESSION_POOL_H
#define HTTP_COMPRESSION_POOL_H

#include "compression/http_encoder.h"

/* Acquire an encoder for the given codec + level. On a hit, the encoder
 * was reset() to a clean state. On a miss (cold pool, or codec without
 * reset support), a fresh encoder is created. NULL on allocation failure. */
http_encoder_t *http_compression_pool_acquire(http_codec_id_t id, int level);

/* Return an encoder to the pool. Encoders without a reset() implementation
 * are destroyed instead of being cached — keeping them would force the
 * next acquire down the same destroy/create cycle. */
void http_compression_pool_release(http_encoder_t *enc);

/* Drain the pool for the current thread. Wired into PHP RSHUTDOWN so each
 * worker releases its codec state before module shutdown. */
void http_compression_pool_shutdown(void);

#endif /* HTTP_COMPRESSION_POOL_H */
