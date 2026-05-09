/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Single file-delivery engine — used by both StaticHandler (`PASSTHROUGH_PHP`
 * on error) and HttpResponse::sendFile (`INLINE_500` on error). Drives the
 * async chain `fs_open → fstat → headers → protocol op → finalize`.
 *
 * The engine is mount-agnostic: per-mount overrides (extra headers, MIME
 * overrides) are read out of `send_file_config_t`; any field left NULL is
 * skipped. Protocol-side coupling lives entirely behind `send_file_cbs_t`
 * — the engine never touches http_connection_t / http1_request_ctx_t /
 * http2_stream_t. */

#ifndef TRUE_ASYNC_SEND_FILE_H
#define TRUE_ASYNC_SEND_FILE_H

#include "php.h"
#include "Zend/zend_types.h"
#include "Zend/zend_string.h"
#include "Zend/zend_hash.h"
#include "php_http_server.h" /* http_server_object, http_server_counters_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "static/http_static_cache.h" /* http_static_cache_view_t */

struct http_request_t;

typedef enum
{
	/* Open failure: synth 500 inline via http_response_emit_status_body.
	 * Internal errors (stat fail, etc): same. — sendFile: caller already
	 * passed a path it expects to exist; failure is server-side. */
	SEND_FILE_ERR_INLINE_500 = 0,
	/* Open failure: rollback to PHP via cbs->on_passthrough; engine
	 * tears down without writing response_obj. Internal errors: emit
	 * 500 via the protocol op. — StaticHandler with on_missing:Next. */
	SEND_FILE_ERR_PASSTHROUGH_PHP = 1,
	/* Open failure: emit 404 via the protocol op. Internal errors: 500
	 * via the op. — StaticHandler default (no on_missing:Next). */
	SEND_FILE_ERR_EMIT_VIA_OP = 2,
} send_file_on_error_t;

/* Per-call configuration. Owned by the caller; the engine takes a
 * shallow copy at kick_off, so all pointer fields must outlive the
 * synchronous tail of send_file(). For zend_string fields, the engine
 * does NOT addref — pin them on the caller side until on_done fires. */
typedef struct
{
	/* Absolute on-disk path. Required. */
	const char *abs_path;
	size_t abs_path_len;

	/* Override headers. NULL → derive from file (MIME-detect for
	 * content_type), or skip emitting (cache_control, content_disposition). */
	const zend_string *content_type;
	const zend_string *content_disposition;
	const zend_string *cache_control;

	/* Feature toggles. */
	bool etag : 1;			   /* emit ETag, honor If-None-Match */
	bool last_modified : 1;	   /* emit Last-Modified, honor If-Modified-Since */
	bool accept_ranges : 1;	   /* emit Accept-Ranges, parse Range / If-Range */
	bool conditional : 1;	   /* short-circuit to 304 when conditions match */

	/* unlink(abs_path) after a successful 2xx. */
	bool delete_after_send : 1;

	/* Caller-resolved precompressed sidecar. When set, abs_path already
	 * points at the sidecar; the engine emits Content-Encoding (token
	 * literal owned by caller) + Vary: Accept-Encoding. NULL = identity. */
	const char *content_encoding;
	size_t content_encoding_len;

	/* 0 = auto (200/206/304). Non-zero = override final status code
	 * unless the engine emits 4xx/5xx. */
	int status_override;

	/* Mount-only — pass NULL from sendFile. Both HashTables are
	 * borrowed; the engine never frees them. */
	const HashTable *extra_headers;	  /* lower-name → IS_STRING value */
	const HashTable *mime_overrides;  /* lower-extension → IS_STRING value */

	/* Pre-rendered open-file-cache snapshot. NULL on cache miss. When
	 * supplied, the engine skips IO_STAT, etag formatting, MIME lookup
	 * and IMF-date formatting — all derived data lives in the view. */
	const http_static_cache_view_t *cache_view;

	/* Telemetry sink. May be NULL on offline tests. */
	http_server_counters_t *counters;

	/* Server pointer for open-file-cache integration on miss path.
	 * NULL = cache disabled (sendFile case). */
	http_server_object *server;

	send_file_on_error_t on_error;
} send_file_config_t;

typedef struct
{
	/* Called once the engine's async chain has kicked off — caller
	 * pins protocol-side resources (refcount conn, bump in-flight
	 * counter). Paired with on_done. NULL = nothing to do. */
	void (*on_armed)(void *user);

	/* Called when delivery finishes — synchronously for the 304/error
	 * inline paths, or async after the protocol op. status==0 ok,
	 * non-zero abort. NULL = nothing to do. */
	void (*on_done)(void *user, int status);

	/* on_error == PASSTHROUGH_PHP only: the engine is asking the caller
	 * to spawn its PHP-handler coroutine. NULL is invalid in that mode. */
	void (*on_passthrough)(void *user);

	/* Per-protocol keep-alive verdict (H1 reads conn->keep_alive,
	 * multiplexed transports return true). NULL → assume true. */
	bool (*keep_alive)(void *user);
} send_file_cbs_t;

typedef enum
{
	/* on_passthrough was fired (PASSTHROUGH_PHP only). */
	SEND_FILE_PASSTHROUGH = 0,
	/* response_obj populated synchronously (304 or 4xx error body). */
	SEND_FILE_HANDLED = 1,
	/* Async chain in flight; on_armed fired, on_done will fire later. */
	SEND_FILE_ASYNC = 2,
} send_file_result_t;

send_file_result_t send_file(struct http_request_t *request, zend_object *response_obj,
							 const send_file_config_t *config, const send_file_cbs_t *cbs,
							 void *user);

#endif
