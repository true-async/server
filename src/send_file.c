/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Single file-delivery engine — protocol-agnostic, mount-agnostic.
 *
 *     open()/fstat() inline → headers → protocol send_static_response → finalize
 *
 * open(2) and fstat(2) execute synchronously on the event-loop thread:
 * for local filesystems both calls are dentry-cache hits in the common
 * case, and the thread-pool round-trip (submit → worker wakes →
 * uv_async back) cost more than the syscalls themselves. Async I/O
 * remains for the protocol op (sendfile / writev).
 *
 * Behaviour decisions (etag / range / conditional / mount-side overrides /
 * inline error vs PHP passthrough) all flow from `send_file_config_t`.
 * Protocol-side coupling lives behind `send_file_cbs_t`. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "Zend/zend_async_API.h"
#include "php_http_server.h"
#include "http1/http_parser.h" /* http_request_t */
#include "http_response_internal.h"
#include "send_file.h"
#include "http_mime.h"
#include "http_etag.h"
#include "http_date.h"
#include "http_conditional.h"
#include "http_range.h"
#include "fs_util.h"
#include "static/static_handler.h" /* http_static_cache_acquire decl */
#include "static/http_static_cache.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>

/* zend_async_API.h ≥ 2026-05-11 exposes ZEND_ASYNC_IO_OWNS_FD, telling
 * the reactor to close crt_fd on dispose for io_t handles created via
 * io_create. Older installed headers omit it but the loaded runtime
 * still honours the bit; define defensively. */
#ifndef ZEND_ASYNC_IO_OWNS_FD
#define ZEND_ASYNC_IO_OWNS_FD (1u << 6)
#endif

typedef struct
{
	send_file_config_t cfg;
	send_file_cbs_t cbs;
	void *user;

	bool armed;

	struct http_request_t *request;
	zend_object *response_obj;

	/* On-disk path. emalloc'd in send_file, freed by state_free. */
	char *fs_path;
	size_t fs_path_len;

	zend_async_io_t *file_io;
	struct stat st;

	bool is_head;

	/* Deep copy of cfg.cache_view when supplied — caller's stack
	 * frame is gone by the time the async tail (protocol op) fires.
	 * The view's inner const char* fields point into the open-file
	 * cache and are stable until eviction, so copying the struct by
	 * value is safe. */
	bool has_cache_view;
	http_static_cache_view_t cache_view_copy;

	/* 0-ms tick that defers engine_handle_stat (or the open-failure
	 * error path) out of the synchronous send_file() tail so on_done
	 * cannot re-enter the request dispatcher on its own call stack. */
	zend_async_timer_event_t *defer_timer;
	zend_async_event_callback_t *defer_cb;
	int defer_status;
	const char *defer_body;
	size_t defer_body_len;
	bool defer_emit_error;

	/* Range support. */
	bool is_range;
	uint64_t range_first;
	uint64_t range_last;
	uint64_t range_total;
} engine_state_t;

typedef struct
{
	zend_async_event_callback_t base;
	engine_state_t *state;
} engine_defer_cb_t;

static inline void engine_state_free(engine_state_t *state)
{
	if (state->fs_path != NULL) {
		efree(state->fs_path);
		state->fs_path = NULL;
	}

	if (state->cfg.content_type != NULL) {
		zend_string_release((zend_string *)state->cfg.content_type);
	}

	if (state->cfg.content_disposition != NULL) {
		zend_string_release((zend_string *)state->cfg.content_disposition);
	}

	if (state->cfg.cache_control != NULL) {
		zend_string_release((zend_string *)state->cfg.cache_control);
	}

	efree(state);
}

static bool engine_keep_alive(const engine_state_t *state)
{
	if (state->cbs.keep_alive != NULL) {
		return state->cbs.keep_alive(state->user);
	}

	return true;
}

/* Apply per-extension MIME override from cfg->mime_overrides if set;
 * otherwise fall back to the builtin. */
static bool engine_resolve_content_type(const engine_state_t *state, const char **out,
										size_t *out_len)
{
	if (state->cfg.content_type != NULL) {
		*out = ZSTR_VAL(state->cfg.content_type);
		*out_len = ZSTR_LEN(state->cfg.content_type);
		return true;
	}

	if (state->cfg.mime_overrides != NULL) {
		char ext[32];
		const size_t ext_len =
			http_mime_extract_lowered_ext(state->fs_path, state->fs_path_len, ext, sizeof(ext));

		if (ext_len > 0) {
			const zval *o = zend_hash_str_find(state->cfg.mime_overrides, ext, ext_len);

			if (o != NULL && Z_TYPE_P(o) == IS_STRING) {
				*out = Z_STRVAL_P(o);
				*out_len = Z_STRLEN_P(o);
				return true;
			}
		}
	}

	return http_mime_lookup_by_ext(state->fs_path, state->fs_path_len, out, out_len);
}

static void engine_on_protocol_done(void *user, int status);

/* Tear down + fire on_done. Used as the tail of engine_on_protocol_done
 * and on synchronous early-error paths that need cleanup before the
 * caller sees us return. status==0 ok, non-zero abort. */
static void engine_defer_cleanup(engine_state_t *state);

static void engine_finalize(engine_state_t *state, int status)
{
	engine_defer_cleanup(state);

	if (state->file_io != NULL) {
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}

		state->file_io = NULL;
	}

	if (state->cfg.delete_after_send && status == 0 && state->fs_path != NULL) {
		(void)unlink(state->fs_path);
	}

	const send_file_cbs_t cbs_copy = state->cbs;
	void *user = state->user;
	const bool armed = state->armed;
	engine_state_free(state);

	if (armed && cbs_copy.on_done != NULL) {
		cbs_copy.on_done(user, status);
	}
}

static bool engine_delegate_to_protocol(engine_state_t *state, zend_async_io_t *file_io,
										uint64_t body_offset, uint64_t body_length, bool head_only)
{
	zend_object *response_obj = state->response_obj;
	const http_response_stream_ops_t *ops = http_response_get_stream_ops(response_obj);
	void *op_ctx = http_response_get_stream_ctx(response_obj);

	if (UNEXPECTED(ops == NULL || ops->send_static_response == NULL)) {
		return false;
	}

	if (file_io == NULL && state->file_io != NULL) {
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}
	}

	state->file_io = NULL;

	const int rc = ops->send_static_response(op_ctx, response_obj, file_io, body_offset, body_length,
											  head_only, engine_on_protocol_done, state);

	if (UNEXPECTED(rc != 0)) {
		state->file_io = file_io;
		return false;
	}

	return true;
}

static bool engine_emit_error_via_op(engine_state_t *state, int status, const char *body,
									 size_t body_len)
{
	zend_object *response_obj = state->response_obj;
	http_response_static_set_status(response_obj, status);
	http_response_static_set_header(response_obj, "content-type", 12, "text/plain; charset=utf-8",
									25);

	if (body != NULL && body_len > 0) {
		http_response_static_set_body_cstr(response_obj, body, body_len);
	}

	http_response_set_content_length(response_obj, (uint64_t)body_len);
	http_response_set_connection(response_obj, engine_keep_alive(state));

	if (state->cfg.counters != NULL) { http_server_count_request(state->cfg.counters); }
	return engine_delegate_to_protocol(state, NULL, 0, 0, true);
}

static void engine_handle_stat(engine_state_t *state)
{
	zend_object *response_obj = state->response_obj;
	const send_file_config_t *cfg = &state->cfg;

	if (UNEXPECTED(!S_ISREG(state->st.st_mode))) {
		const int status = (cfg->on_error == SEND_FILE_ERR_INLINE_500) ? 500 : 404;
		const char *body = (status == 500) ? "Internal Server Error" : "Not Found";
		const size_t body_len = (status == 500) ? 21 : 9;

		if (!engine_emit_error_via_op(state, status, body, body_len)) {
			engine_finalize(state, -1);
		}

		return;
	}

	/* === Build derived metadata (cache hit short-circuits) =========== */

	char etag_buf[HTTP_ETAG_BUF_LEN];
	char last_modified_buf[HTTP_DATE_BUF_LEN];
	bool etag_enabled = cfg->etag;
	const char *content_type = NULL;
	size_t content_type_len = 0;
	const http_static_cache_view_t *view =
		state->has_cache_view ? &state->cache_view_copy : NULL;

	if (view != NULL) {
		etag_enabled = view->etag != NULL && view->etag_len == HTTP_ETAG_LEN;

		if (etag_enabled) {
			memcpy(etag_buf, view->etag, HTTP_ETAG_LEN);
			etag_buf[HTTP_ETAG_LEN] = '\0';
		}

		if (view->last_modified != NULL && view->last_modified_len == HTTP_DATE_LEN) {
			memcpy(last_modified_buf, view->last_modified, HTTP_DATE_LEN);
			last_modified_buf[HTTP_DATE_LEN] = '\0';
		} else if (cfg->last_modified) {
			http_date_format_imf(state->st.st_mtime, last_modified_buf);
		}

		content_type = view->content_type;
		content_type_len = view->content_type_len;
	} else {
		if (etag_enabled) {
			http_etag_format_strong(&state->st, etag_buf);
		}

		if (cfg->last_modified) {
			http_date_format_imf(state->st.st_mtime, last_modified_buf);
		}

		if (!engine_resolve_content_type(state, &content_type, &content_type_len)) {
			content_type = "application/octet-stream";
			content_type_len = sizeof("application/octet-stream") - 1;
		}
	}

	/* === Conditional GET (304) ====================================== */

	bool not_modified = false;

	if (cfg->conditional) {
		const zend_string *if_none_match =
			http_request_find_header(state->request, "if-none-match", 13);
		const zend_string *if_modified_since =
			http_request_find_header(state->request, "if-modified-since", 17);
		not_modified = http_conditional_check(
			if_none_match != NULL ? ZSTR_VAL(if_none_match) : NULL,
			if_none_match != NULL ? ZSTR_LEN(if_none_match) : 0,
			if_modified_since != NULL ? ZSTR_VAL(if_modified_since) : NULL,
			if_modified_since != NULL ? ZSTR_LEN(if_modified_since) : 0,
			etag_enabled ? etag_buf : NULL, etag_enabled ? HTTP_ETAG_LEN : 0, state->st.st_mtime);
	}

	/* === Cache insert (only on miss path) =========================== */

	if (view == NULL && cfg->server != NULL) {
		http_static_cache_t *cache = http_static_cache_acquire(cfg->server);

		if (cache != NULL) {
			http_static_cache_insert(cache, state->fs_path, state->fs_path_len, &state->st,
									 content_type, content_type_len,
									 etag_enabled ? etag_buf : NULL,
									 etag_enabled ? HTTP_ETAG_LEN : 0,
									 cfg->last_modified ? last_modified_buf : NULL,
									 cfg->last_modified ? HTTP_DATE_LEN : 0);
		}
	}

	/* === Range support (RFC 9110 §14.2) ============================= */

	state->range_total = (uint64_t)state->st.st_size;
	state->is_range = false;

	if (!not_modified && cfg->accept_ranges) {
		const zend_string *range_hdr = http_request_find_header(state->request, "range", 5);
		const zend_string *if_range = http_request_find_header(state->request, "if-range", 8);
		bool range_allowed = true;

		if (if_range != NULL && etag_enabled) {
			range_allowed = (ZSTR_LEN(if_range) == HTTP_ETAG_LEN &&
							 memcmp(ZSTR_VAL(if_range), etag_buf, HTTP_ETAG_LEN) == 0);
		} else if (if_range != NULL) {
			range_allowed = false;
		}

		if (range_hdr != NULL && range_allowed) {
			uint64_t first = 0, last = 0;
			const http_range_result_t rc = http_range_parse(
				ZSTR_VAL(range_hdr), ZSTR_LEN(range_hdr), state->range_total, &first, &last);

			if (rc == HTTP_RANGE_OK) {
				state->is_range = true;
				state->range_first = first;
				state->range_last = last;
			} else if (rc == HTTP_RANGE_NOT_SATISFIABLE) {
				http_response_static_set_status(response_obj, 416);
				http_response_static_set_header(response_obj, "content-type", 12,
												"text/plain; charset=utf-8", 25);
				char cr[48];
				const int crn = snprintf(cr, sizeof(cr), "bytes */%" PRIu64, state->range_total);

				if (crn > 0 && (size_t)crn < sizeof(cr)) {
					http_response_static_set_header(response_obj, "content-range", 13, cr,
													(size_t)crn);
				}

				http_response_static_set_header(response_obj, "content-length", 14, "0", 1);
				http_response_set_connection(response_obj, engine_keep_alive(state));

				if (cfg->counters != NULL) { http_server_count_request(cfg->counters); }

				if (!engine_delegate_to_protocol(state, NULL, 0, 0, true)) {
					engine_finalize(state, -1);
				}

				return;
			}
		}
	}

	const bool keep_alive = engine_keep_alive(state);

	/* === Build response headers ===================================== */

	const bool include_content_headers = !not_modified;
	int status_code = not_modified ? 304 : (state->is_range ? 206 : 200);

	if (cfg->status_override != 0 && !not_modified && status_code < 400) {
		status_code = cfg->status_override;
	}

	http_response_static_set_status(response_obj, status_code);

	if (include_content_headers && content_type != NULL) {
		http_response_static_set_header(response_obj, "content-type", 12, content_type,
										content_type_len);
	}

	const uint64_t body_len = not_modified
								  ? 0
								  : (state->is_range ? (state->range_last - state->range_first + 1)
													 : (uint64_t)state->st.st_size);

	if (include_content_headers) {
		http_response_set_content_length(response_obj, body_len);
	}

	if (etag_enabled) {
		http_response_static_set_header(response_obj, "etag", 4, etag_buf, HTTP_ETAG_LEN);
	}

	if (cfg->last_modified) {
		http_response_static_set_header(response_obj, "last-modified", 13, last_modified_buf,
										HTTP_DATE_LEN);
	}

	if (cfg->content_encoding != NULL && include_content_headers) {
		http_response_static_set_header(response_obj, "content-encoding", 16, cfg->content_encoding,
										cfg->content_encoding_len);
	}

	if (cfg->content_encoding != NULL) {
		http_response_static_set_header(response_obj, "vary", 4, "Accept-Encoding", 15);
	}

	if (cfg->accept_ranges && include_content_headers) {
		http_response_static_set_header(response_obj, "accept-ranges", 13, "bytes", 5);
	}

	if (state->is_range && include_content_headers) {
		char cr[64];
		const int n = snprintf(cr, sizeof(cr), "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64,
							   state->range_first, state->range_last, state->range_total);

		if (n > 0 && (size_t)n < sizeof(cr)) {
			http_response_static_set_header(response_obj, "content-range", 13, cr, (size_t)n);
		}
	}

	if (cfg->cache_control != NULL) {
		http_response_static_set_header(response_obj, "cache-control", 13,
										ZSTR_VAL(cfg->cache_control),
										ZSTR_LEN(cfg->cache_control));
	}

	if (cfg->content_disposition != NULL) {
		http_response_static_set_header(response_obj, "content-disposition", 19,
										ZSTR_VAL(cfg->content_disposition),
										ZSTR_LEN(cfg->content_disposition));
	}

	http_response_apply_extra_headers(response_obj, cfg->extra_headers, include_content_headers);
	http_response_set_connection(response_obj, keep_alive);

	/* === Hand off to the protocol op ================================= */

	zend_async_io_t *file_io = state->file_io;

	if (not_modified) {
		if (cfg->counters != NULL) { http_server_count_request(cfg->counters); }

		if (UNEXPECTED(!engine_delegate_to_protocol(state, file_io, 0, 0, true))) {
			engine_finalize(state, -1);
		}

		return;
	}

	if (state->is_head || state->st.st_size == 0) {
		if (cfg->counters != NULL) { http_server_count_request(cfg->counters); }

		if (UNEXPECTED(!engine_delegate_to_protocol(state, file_io, 0, 0, true))) {
			engine_finalize(state, -1);
		}

		return;
	}

	/* === Small-file fast path (slurp + inline body) ================== */
	/* libuv's uv_fs_sendfile is doubly broken for file→TCP-socket: it
	 * tries copy_file_range first (returns EINVAL on socket), then
	 * falls into a userspace pread+write loop *inside* a worker
	 * thread — no kernel zero-copy AND a thread-pool round-trip per
	 * request. For small files the round-trip dominates. Bypass it:
	 * read the whole file synchronously into an emalloc'd
	 * zend_string, hand the body to the response object, and
	 * delegate with file_io=NULL — the protocol op then emits a
	 * single writev(headers + inline body) via the same per-socket
	 * write queue that headers normally use. Ordering with prior
	 * writes on the same socket is guaranteed by libuv's stream
	 * write queue. */
#define SEND_FILE_SLURP_THRESHOLD ((size_t)64 * 1024)

	if (!state->is_range && (size_t)state->st.st_size <= SEND_FILE_SLURP_THRESHOLD &&
		file_io != NULL) {
		zend_string *body = fs_slurp_fd((int)file_io->descriptor.fd, (size_t)state->st.st_size);

		if (body != NULL) {
			http_response_static_set_body_str(response_obj, body);
			zend_string_release(body);

			if (cfg->counters != NULL) { http_server_count_request(cfg->counters); }

			/* file_io=NULL → engine_delegate_to_protocol disposes
			 * state->file_io and the protocol op writes the inline
			 * body riding along with headers. */
			if (UNEXPECTED(!engine_delegate_to_protocol(state, NULL, 0, 0, true))) {
				engine_finalize(state, -1);
			}

			return;
		}
		/* Slurp failed — fall through to sendfile path. */
	}

	const uint64_t body_offset = state->is_range ? state->range_first : 0;

	if (cfg->counters != NULL) { http_server_count_request(cfg->counters); }

	if (UNEXPECTED(!engine_delegate_to_protocol(state, file_io, body_offset, body_len, false))) {
		engine_finalize(state, -1);
	}
}

static void engine_on_protocol_done(void *user, int status)
{
	engine_state_t *state = (engine_state_t *)user;

	if (state == NULL) {
		return;
	}

	engine_finalize(state, status);
}

static void engine_defer_cb_dispose(zend_async_event_callback_t *cb, zend_async_event_t *event)
{
	(void)event;
	efree(cb);
}

/* Tear down the deferral timer + its callback. Used both on normal fire
 * (one-shot timer disposes itself after the callback returns) and on
 * cleanup paths where we never made it to the fire. */
static void engine_defer_cleanup(engine_state_t *state)
{
	if (state->defer_cb != NULL && state->defer_timer != NULL) {
		(void)state->defer_timer->base.del_callback(&state->defer_timer->base, state->defer_cb);
		state->defer_cb = NULL;
	}

	if (state->defer_timer != NULL) {
		if (state->defer_timer->base.dispose != NULL) {
			state->defer_timer->base.dispose(&state->defer_timer->base);
		}

		state->defer_timer = NULL;
	}
}

/* Timer fire: run the deferred work, drop the timer. */
static void engine_defer_dispatch(zend_async_event_t *event, zend_async_event_callback_t *callback,
								  void *result, zend_object *exception)
{
	(void)event;
	(void)result;
	(void)exception;
	engine_state_t *state = ((engine_defer_cb_t *)callback)->state;
	engine_defer_cleanup(state);

	if (state->defer_emit_error) {
		if (!engine_emit_error_via_op(state, state->defer_status, state->defer_body,
									   state->defer_body_len)) {
			engine_finalize(state, -1);
		}

		return;
	}

	engine_handle_stat(state);
}

/* Park `state` on a 0-ms timer. The reactor wakes us on the next loop
 * iteration, ensuring on_armed / engine_handle_stat / on_done can never
 * unwind through the synchronous send_file() call stack. Returns false
 * on allocation failure; caller surfaces SEND_FILE_HANDLED. */
static bool engine_defer_schedule(engine_state_t *state)
{
	zend_async_timer_event_t *timer = ZEND_ASYNC_NEW_TIMER_EVENT(0, false);

	if (UNEXPECTED(timer == NULL)) {
		return false;
	}

	engine_defer_cb_t *cb = (engine_defer_cb_t *)ZEND_ASYNC_EVENT_CALLBACK_EX(engine_defer_dispatch,
																			   sizeof(engine_defer_cb_t));

	if (UNEXPECTED(cb == NULL)) {
		timer->base.dispose(&timer->base);
		return false;
	}

	cb->base.dispose = engine_defer_cb_dispose;
	cb->state = state;

	if (UNEXPECTED(!timer->base.add_callback(&timer->base, &cb->base))) {
		efree(cb);
		timer->base.dispose(&timer->base);
		return false;
	}

	if (UNEXPECTED(!timer->base.start(&timer->base))) {
		(void)timer->base.del_callback(&timer->base, &cb->base);
		timer->base.dispose(&timer->base);
		return false;
	}

	state->defer_timer = timer;
	state->defer_cb = &cb->base;
	return true;
}

/* Schedule a deferred error emission via the protocol op. on_armed
 * fires synchronously here (caller pins resources), the actual head
 * is written on the next loop tick. */
static send_file_result_t engine_arm_and_defer_error(engine_state_t *state, int status,
													  const char *body, size_t body_len)
{
	if (state->cbs.on_armed != NULL) {
		state->cbs.on_armed(state->user);
	}

	state->armed = true;
	state->defer_emit_error = true;
	state->defer_status = status;
	state->defer_body = body;
	state->defer_body_len = body_len;

	if (UNEXPECTED(!engine_defer_schedule(state))) {
		engine_finalize(state, -1);
		return SEND_FILE_ASYNC;
	}

	return SEND_FILE_ASYNC;
}

/* Handle an open() failure on the synchronous prologue. Either rolls
 * back to PHP (PASSTHROUGH_PHP) or emits 404/500 head via the protocol
 * op (deferred). PASSTHROUGH_PHP is fired synchronously since the
 * caller (request dispatcher) handles it via the explicit
 * SEND_FILE_PASSTHROUGH return code, not via on_done re-entry. */
static send_file_result_t engine_handle_open_failure(engine_state_t *state)
{
	const send_file_config_t *cfg = &state->cfg;

	if (cfg->on_error == SEND_FILE_ERR_PASSTHROUGH_PHP) {
		const send_file_cbs_t cbs_copy = state->cbs;
		void *user = state->user;
		engine_state_free(state);

		if (cbs_copy.on_passthrough != NULL) {
			cbs_copy.on_passthrough(user);
		}

		return SEND_FILE_PASSTHROUGH;
	}

	const bool is_404 = (cfg->on_error == SEND_FILE_ERR_EMIT_VIA_OP);
	const int status = is_404 ? 404 : 500;
	const char *body = is_404 ? "Not Found" : "Internal Server Error";
	const size_t body_len = is_404 ? 9 : 21;

	return engine_arm_and_defer_error(state, status, body, body_len);
}

send_file_result_t send_file(struct http_request_t *request, zend_object *response_obj,
							 const send_file_config_t *config, const send_file_cbs_t *cbs,
							 void *user)
{
	if (UNEXPECTED(request == NULL || response_obj == NULL || config == NULL ||
				   config->abs_path == NULL || config->abs_path_len == 0)) {
		return SEND_FILE_HANDLED;
	}

	if (UNEXPECTED(config->abs_path_len + 1 >= MAXPATHLEN)) {
		return SEND_FILE_HANDLED;
	}

	const http_response_stream_ops_t *ops = http_response_get_stream_ops(response_obj);

	if (UNEXPECTED(ops == NULL || ops->send_static_response == NULL)) {
		/* Caller's responsibility — engine refuses to drive without a
		 * protocol op. Either the adapter falls back to a synchronous
		 * path or it surfaces 500. */
		return SEND_FILE_HANDLED;
	}

	engine_state_t *state = ecalloc(1, sizeof(*state));
	state->cfg = *config;

	if (cbs != NULL) {
		state->cbs = *cbs;
	}

	state->user = user;
	state->request = request;
	state->response_obj = response_obj;
	state->fs_path_len = config->abs_path_len;
	state->fs_path = emalloc(config->abs_path_len + 1);
	memcpy(state->fs_path, config->abs_path, config->abs_path_len);
	state->fs_path[config->abs_path_len] = '\0';
	state->is_head = http_request_method_is_head(request);

	if (config->cache_view != NULL) {
		state->cache_view_copy = *config->cache_view;
		state->has_cache_view = true;
	}

	/* The cfg snapshot is shallow — caller's stack frame may be gone
	 * by the time the async tail (protocol op) fires. Pin every
	 * zend_string we intend to read past kick-off; finalize releases. */
	if (state->cfg.content_type != NULL) {
		zend_string_addref((zend_string *)state->cfg.content_type);
	}

	if (state->cfg.content_disposition != NULL) {
		zend_string_addref((zend_string *)state->cfg.content_disposition);
	}

	if (state->cfg.cache_control != NULL) {
		zend_string_addref((zend_string *)state->cfg.cache_control);
	}

	/* === Synchronous open(2) ========================================= */

	const int fd = open(state->fs_path, O_RDONLY | O_CLOEXEC);

	if (UNEXPECTED(fd < 0)) {
		return engine_handle_open_failure(state);
	}

	/* === Synchronous fstat(2) (skipped on cache hit) ================= */

	if (state->has_cache_view) {
		state->st = state->cache_view_copy.st;
	} else if (UNEXPECTED(fstat(fd, &state->st) < 0)) {
		(void)close(fd);
		return engine_arm_and_defer_error(state, 500, "Internal Server Error", 21);
	}

	/* Wrap the fd into an async io_t handle. Initial state mirrors what
	 * fs_open emits after its open callback fires: READABLE for
	 * sendfile/stat/seek, OWNS_FD so dispose closes the fd through the
	 * reactor's uv_fs_close path. */
	state->file_io = ZEND_ASYNC_IO_CREATE((zend_file_descriptor_t)fd, ZEND_ASYNC_IO_TYPE_FILE,
										   ZEND_ASYNC_IO_READABLE | ZEND_ASYNC_IO_OWNS_FD);

	if (UNEXPECTED(state->file_io == NULL)) {
		(void)close(fd);
		return engine_arm_and_defer_error(state, 500, "Internal Server Error", 21);
	}

	if (state->cbs.on_armed != NULL) {
		state->cbs.on_armed(user);
	}

	state->armed = true;

	if (UNEXPECTED(!engine_defer_schedule(state))) {
		engine_finalize(state, -1);
		return SEND_FILE_ASYNC;
	}

	return SEND_FILE_ASYNC;
}
