/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP2_STATIC_ACCOUNTING_H
#define HTTP2_STATIC_ACCOUNTING_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "core/http_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-worker counters live in http2_static_response.c (ZEND_TLS = static
 * TSRM_TLS, so no extern). Cross-TU consumers only need the release
 * wrappers below — accounting stays opaque. h2_static_telemetry_*
 * accessors can be added here later if /metrics needs them. */

/* Drop-in wrappers around zend_string_release for chunks owned by the
 * h2 static FSM. The non-stream variant is used by h2_static_finalize
 * for pending_chunk (no stream attachment); the stream variant is used
 * by every chunk-queue drain site (h2_emit_streaming_body,
 * h2_dp_streaming_copy, stream_dtor) — it no-ops accounting when the
 * stream's chunks are user-streaming chunks rather than static. */
void h2_static_account_release_pending(http_connection_t *conn,
                                       zend_string *chunk);

/* Release a chunk drained from stream->chunk_queue. If the stream is
 * static-tracked, debits both the per-conn and per-worker counters
 * before zend_string_release; otherwise just releases. Wakes any
 * memory-throttled FSMs if the global counter crossed the resume
 * threshold. */
void h2_static_account_release_chunk(struct http2_stream_t *stream,
                                     zend_string *chunk);

#ifdef __cplusplus
}
#endif

#endif /* HTTP2_STATIC_ACCOUNTING_H */
