/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Sendfile FSM for HttpResponse::sendFile() — parallel to the static-
 * mount FSM in src/static/http_static.c but driven by a per-call
 * SendFileOptions snapshot instead of a mount descriptor.
 *
 *   fs_open → fstat → headers → send_static_response (op) → finalize
 *
 * The protocol-side op (h1_stream_send_static_response /
 * h2_stream_send_static_response) does the actual head + body bytes;
 * we just stage the response object's headers from the options +
 * stat info, then transfer ownership of the open file_io to the op. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "Zend/zend_async_API.h"
#include "php_http_server.h"
#include "http1/http_parser.h"
#include "http_response_internal.h"
#include "http_send_file.h"
#include "static/static_handler.h"
#include "http_mime.h"
#include "http_etag.h"
#include "http_date.h"
#include "http_conditional.h"
#include "http_range.h"
#include "http_rfc5987.h"
#include "compression/http_compression_negotiate.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef enum {
	SF_PHASE_OPEN = 0,
	SF_PHASE_STAT,
	SF_PHASE_DONE,
} sf_phase_t;

typedef struct {
	http_request_t          *request;
	zend_object             *response_obj;

	http_send_file_request_t *req;   /* owned, freed in finalize */

	/* Sidecar-resolved fs path. Initially the user-supplied absolute
	 * path; rewritten to "<path>.gz" / ".br" / ".zst" when a
	 * precompressed sibling matches Accept-Encoding. */
	char                    *fs_path;
	size_t                   fs_path_len;

	/* Encoding chosen for the sidecar (NULL = identity). Token literal
	 * owned by the codec table — never freed. */
	const char              *content_encoding;
	size_t                   content_encoding_len;

	/* MIME from the original (pre-rewrite) path. Borrowed pointer (built-
	 * in MIME table or constant) — never freed. */
	const char              *content_type;
	size_t                   content_type_len;

	zend_async_io_t         *file_io;
	struct stat              st;

	bool                     is_head;

	sf_phase_t               phase;
	zend_async_io_req_t     *pending_req;
	zend_async_event_callback_t *cb;

	void                   (*on_done)(void *user, int status);
	void                    *user;
} sf_state_t;

typedef struct {
	zend_async_event_callback_t base;
	sf_state_t                  *state;
} sf_cb_t;

static void sf_cb_dispose(zend_async_event_callback_t *cb,
                          zend_async_event_t *event)
{
	(void)event;
	efree(cb);
}

static inline void sf_state_free(sf_state_t *state)
{
	if (state == NULL) return;
	if (state->fs_path != NULL) {
		efree(state->fs_path);
		state->fs_path = NULL;
	}
	if (state->req != NULL) {
		http_send_file_request_free(state->req);
		state->req = NULL;
	}
	efree(state);
}

/* Best-effort RFC 5987 "filename*=UTF-8''..." encode. ASCII-clean
 * names also get a plain filename="" form so legacy clients work. */
static void sf_set_content_disposition(zend_object *r,
                                       const http_send_file_options_t *opts)
{
	const bool is_attachment =
		(opts->disposition == HTTP_SEND_FILE_DISPOSITION_ATTACHMENT)
		|| (opts->download_name != NULL && !opts->disposition_set);

	if (!is_attachment && opts->download_name == NULL) {
		return;
	}

	smart_str s = {0};
	smart_str_appends(&s, is_attachment ? "attachment" : "inline");

	if (opts->download_name != NULL) {
		const char *const dn = ZSTR_VAL(opts->download_name);
		const size_t dn_len  = ZSTR_LEN(opts->download_name);

		bool ascii_clean = true;
		for (size_t i = 0; i < dn_len; i++) {
			const unsigned char c = (unsigned char)dn[i];
			if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\') {
				ascii_clean = false;
				break;
			}
		}

		if (ascii_clean) {
			smart_str_appendl(&s, "; filename=\"", 12);
			smart_str_appendl(&s, dn, dn_len);
			smart_str_appendc(&s, '"');
		} else {
			smart_str_appendl(&s, "; filename*=UTF-8''", 19);
			http_rfc5987_encode(&s, dn, dn_len);
		}
	}

	smart_str_0(&s);
	http_response_static_set_header(r, "content-disposition", 19,
	                                ZSTR_VAL(s.s), ZSTR_LEN(s.s));
	smart_str_free(&s);
}

/* Mirror of try_select_precompressed in http_static.c — fewer branches
 * since we only have the "user opted in" toggle, no per-codec gate. */
static bool sf_try_precompressed(sf_state_t *state)
{
	if (!state->req->opts.precompressed) {
		return false;
	}
	if (state->request == NULL || state->request->headers == NULL) {
		return false;
	}

	const zval *const ae_zv = zend_hash_str_find(state->request->headers,
	                                              "accept-encoding", 15);
	if (ae_zv == NULL) {
		return false;
	}
	const zend_string *ae = NULL;
	if (Z_TYPE_P(ae_zv) == IS_STRING) {
		ae = Z_STR_P(ae_zv);
	} else if (Z_TYPE_P(ae_zv) == IS_ARRAY) {
		const zval *const first = zend_hash_index_find(Z_ARRVAL_P(ae_zv), 0);
		if (first != NULL && Z_TYPE_P(first) == IS_STRING) ae = Z_STR_P(first);
	}
	if (ae == NULL) return false;

	http_accept_encoding_t parsed;
	http_accept_encoding_parse(ZSTR_VAL(ae), ZSTR_LEN(ae), &parsed);

	static const struct {
		const char *suffix; size_t suffix_len;
		const char *token;  size_t token_len;
	} codecs[] = {
		{ ".zst", 4, "zstd", 4 },
		{ ".br",  3, "br",   2 },
		{ ".gz",  3, "gzip", 4 },
	};
	const bool acceptable[] = {
		parsed.zstd_acceptable,
		parsed.brotli_acceptable,
		parsed.gzip_acceptable,
	};

	for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
		if (!acceptable[i]) continue;

		char candidate[MAXPATHLEN];
		if (state->fs_path_len + codecs[i].suffix_len + 1 > sizeof(candidate)) {
			continue;
		}
		memcpy(candidate, state->fs_path, state->fs_path_len);
		memcpy(candidate + state->fs_path_len, codecs[i].suffix, codecs[i].suffix_len);
		candidate[state->fs_path_len + codecs[i].suffix_len] = '\0';

		struct stat st;
		if (stat(candidate, &st) != 0) continue;
		if (!S_ISREG(st.st_mode)) continue;

		/* Rewrite fs_path to point at the sibling. */
		efree(state->fs_path);
		state->fs_path_len = state->fs_path_len + codecs[i].suffix_len;
		state->fs_path = emalloc(state->fs_path_len + 1);
		memcpy(state->fs_path, candidate, state->fs_path_len + 1);
		state->content_encoding     = codecs[i].token;
		state->content_encoding_len = codecs[i].token_len;
		return true;
	}
	return false;
}

/* Tear down the FSM. Disposes file_io if still owned, fires on_done. */
static void sf_finalize(sf_state_t *state, int status)
{
	state->phase = SF_PHASE_DONE;

	if (state->cb != NULL && state->file_io != NULL) {
		(void)state->file_io->event.del_callback(&state->file_io->event,
		                                          state->cb);
		state->cb = NULL;
	}
	if (state->file_io != NULL) {
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}
		state->file_io = NULL;
	}

	void (*const on_done)(void *, int) = state->on_done;
	void *const user = state->user;

	/* Best-effort delete on success only. The unlink runs from event-
	 * loop callback context but the path already passed fstat — a
	 * blocking unlink on a regular-file path is microseconds. */
	if (status == 0 && state->req != NULL && state->req->path != NULL
	    && state->req->opts.delete_after_send) {
		(void)unlink(ZSTR_VAL(state->req->path));
	}

	sf_state_free(state);

	if (on_done != NULL) {
		on_done(user, status);
	}
}

/* Protocol op completion. */
static void sf_on_protocol_done(void *user, int status)
{
	sf_state_t *const state = (sf_state_t *)user;
	if (state == NULL) return;
	sf_finalize(state, status);
}

/* Hand off to the protocol's send_static_response op. Mirrors
 * static_fsm_delegate_to_protocol — caller must NOT touch state after
 * a synchronous-complete op. */
static bool sf_delegate(sf_state_t *state, zend_async_io_t *file_io,
                        uint64_t body_offset, uint64_t body_length,
                        bool head_only)
{
	zend_object *const r = state->response_obj;
	const http_response_stream_ops_t *const ops = http_response_get_stream_ops(r);
	void *const op_ctx = http_response_get_stream_ctx(r);

	if (UNEXPECTED(ops == NULL || ops->send_static_response == NULL)) {
		return false;
	}

	if (state->cb != NULL && state->file_io != NULL) {
		(void)state->file_io->event.del_callback(&state->file_io->event,
		                                          state->cb);
		state->cb = NULL;
	}
	if (file_io == NULL && state->file_io != NULL) {
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}
	}

	state->phase = SF_PHASE_DONE;
	state->file_io = NULL;

	const int rc = ops->send_static_response(op_ctx, r, file_io, body_offset,
	                                          body_length, head_only,
	                                          sf_on_protocol_done, state);

	/* On rc==0 + sync-complete on_done, `state` may already be freed. */
	if (UNEXPECTED(rc != 0)) {
		state->file_io = file_io;
		state->phase = SF_PHASE_DONE;
		return false;
	}
	return true;
}

/* Emit a small inline error through the protocol op (head-only). */
static bool sf_emit_inline_error(sf_state_t *state, int status,
                                 const char *body, size_t body_len)
{
	zend_object *const r = state->response_obj;
	http_response_emit_status_body(r, status, body, body_len);
	http_response_set_content_length(r, (uint64_t)body_len);
	http_response_set_connection(r, true);
	return sf_delegate(state, NULL, 0, 0, true);
}

/* Build all response headers from options + stat. */
static void sf_handle_stat(sf_state_t *state)
{
	zend_object *const r = state->response_obj;
	const http_send_file_options_t *const opts = &state->req->opts;

	if (UNEXPECTED(!S_ISREG(state->st.st_mode))) {
		if (!sf_emit_inline_error(state, 500, "sendFile: not a regular file", 28)) {
			sf_finalize(state, -1);
		}
		return;
	}
	if (UNEXPECTED((uint64_t)state->st.st_size > (uint64_t)HTTP_STATIC_MAX_FILE_SIZE)) {
		if (!sf_emit_inline_error(state, 500, "sendFile: file too large", 24)) {
			sf_finalize(state, -1);
		}
		return;
	}

	/* MIME — caller override > built-in lookup. The lookup is keyed on
	 * the *original* path (state->fs_path may have been rewritten to a
	 * .gz/.br/.zst sidecar; the original CT is captured pre-rewrite). */
	const char *content_type;
	size_t content_type_len;
	if (opts->content_type != NULL) {
		content_type     = ZSTR_VAL(opts->content_type);
		content_type_len = ZSTR_LEN(opts->content_type);
	} else if (state->content_type != NULL) {
		content_type     = state->content_type;
		content_type_len = state->content_type_len;
	} else {
		content_type     = "application/octet-stream";
		content_type_len = sizeof("application/octet-stream") - 1;
	}

	/* ETag + Last-Modified scratch. */
	char etag_buf[HTTP_ETAG_BUF_LEN];
	if (opts->etag) {
		http_etag_format_strong(&state->st, etag_buf);
	}
	char lm_buf[HTTP_DATE_BUF_LEN];
	if (opts->last_modified) {
		http_date_format_imf(state->st.st_mtime, lm_buf);
	}

	/* Conditional GET. */
	bool not_modified = false;
	if (opts->conditional && (opts->etag || opts->last_modified)) {
		const zend_string *inm =
			http_request_find_header(state->request, "if-none-match", 13);
		const zend_string *ims =
			http_request_find_header(state->request, "if-modified-since", 17);
		not_modified = http_conditional_check(
			inm != NULL ? ZSTR_VAL(inm) : NULL,
			inm != NULL ? ZSTR_LEN(inm) : 0,
			ims != NULL ? ZSTR_VAL(ims) : NULL,
			ims != NULL ? ZSTR_LEN(ims) : 0,
			opts->etag ? etag_buf : NULL,
			opts->etag ? HTTP_ETAG_LEN : 0,
			state->st.st_mtime);
	}

	const uint64_t total = (uint64_t)state->st.st_size;
	bool is_range = false;
	uint64_t range_first = 0, range_last = 0;
	if (!not_modified && opts->accept_ranges) {
		const zend_string *rh = http_request_find_header(state->request, "range", 5);
		const zend_string *ifr = http_request_find_header(state->request, "if-range", 8);
		bool range_allowed = true;
		if (ifr != NULL && opts->etag) {
			range_allowed = (ZSTR_LEN(ifr) == HTTP_ETAG_LEN
				&& memcmp(ZSTR_VAL(ifr), etag_buf, HTTP_ETAG_LEN) == 0);
		} else if (ifr != NULL) {
			range_allowed = false;
		}
		if (rh != NULL && range_allowed) {
			const http_range_result_t rr = http_range_parse(
				ZSTR_VAL(rh), ZSTR_LEN(rh), total, &range_first, &range_last);
			/* sendFile() does not synthesize 416 — fall back to 200
			 * with the full body on any non-OK outcome (matches the
			 * pre-extraction behavior). */
			if (rr == HTTP_RANGE_OK) {
				is_range = true;
			}
		}
	}

	const bool include_content_headers = !not_modified;
	const int default_status = not_modified ? 304 : (is_range ? 206 : 200);
	const int status_code = (opts->status > 0) ? opts->status : default_status;

	http_response_static_set_status(r, status_code);

	if (include_content_headers) {
		http_response_static_set_header(r, "content-type", 12, content_type, content_type_len);
	}

	const uint64_t body_len = not_modified
		? 0
		: (is_range ? (range_last - range_first + 1) : total);

	if (include_content_headers) {
		http_response_set_content_length(r, body_len);
	}

	if (opts->etag) {
		http_response_static_set_header(r, "etag", 4, etag_buf, HTTP_ETAG_LEN);
	}
	if (opts->last_modified) {
		http_response_static_set_header(r, "last-modified", 13, lm_buf, HTTP_DATE_LEN);
	}

	if (state->content_encoding != NULL && include_content_headers) {
		http_response_static_set_header(r, "content-encoding", 16,
			state->content_encoding, state->content_encoding_len);
	}
	if (state->content_encoding != NULL) {
		http_response_static_set_header(r, "vary", 4, "Accept-Encoding", 15);
	}

	if (opts->accept_ranges && include_content_headers) {
		http_response_static_set_header(r, "accept-ranges", 13, "bytes", 5);
	}
	if (is_range && include_content_headers) {
		char cr[64];
		const int n = snprintf(cr, sizeof(cr),
			"bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64,
			range_first, range_last, total);
		if (n > 0 && (size_t)n < sizeof(cr)) {
			http_response_static_set_header(r, "content-range", 13, cr, (size_t)n);
		}
	}

	if (opts->cache_control != NULL) {
		http_response_static_set_header(r, "cache-control", 13,
			ZSTR_VAL(opts->cache_control), ZSTR_LEN(opts->cache_control));
	}

	sf_set_content_disposition(r, opts);

	http_response_set_connection(r, true);

	zend_async_io_t *const file_io = state->file_io;

	if (not_modified) {
		if (UNEXPECTED(!sf_delegate(state, file_io, 0, 0, true))) {
			sf_finalize(state, -1);
		}
		return;
	}
	if (state->is_head || total == 0) {
		if (UNEXPECTED(!sf_delegate(state, file_io, 0, 0, true))) {
			sf_finalize(state, -1);
		}
		return;
	}
	const uint64_t off = is_range ? range_first : 0;
	if (UNEXPECTED(!sf_delegate(state, file_io, off, body_len, false))) {
		sf_finalize(state, -1);
	}
}

static void sf_handle_open(sf_state_t *state, zend_object *exception)
{
	if (UNEXPECTED(exception != NULL
	               || (state->file_io->state & ZEND_ASYNC_IO_CLOSED) != 0)) {
		if (!sf_emit_inline_error(state, 500, "sendFile: open failed", 21)) {
			sf_finalize(state, -1);
		}
		return;
	}

	state->phase = SF_PHASE_STAT;
	state->pending_req = ZEND_ASYNC_IO_STAT(state->file_io, &state->st);
	if (UNEXPECTED(state->pending_req == NULL)) {
		if (!sf_emit_inline_error(state, 500, "sendFile: stat failed", 21)) {
			sf_finalize(state, -1);
		}
	}
}

static void sf_dispatch(zend_async_event_t *event,
                        zend_async_event_callback_t *callback,
                        void *result, zend_object *exception)
{
	(void)event;
	sf_state_t *const state = ((sf_cb_t *)callback)->state;
	zend_async_io_req_t *const req = (zend_async_io_req_t *)result;

	switch (state->phase) {
	case SF_PHASE_OPEN:
		if (req != NULL) return; /* libuv_fs_open notifies with NULL */
		sf_handle_open(state, exception);
		return;

	case SF_PHASE_STAT:
		if (req == NULL || req != state->pending_req) return;
		state->pending_req = NULL;
		if (req->dispose != NULL) req->dispose(req);
		if (UNEXPECTED(exception != NULL)) {
			if (!sf_emit_inline_error(state, 500, "sendFile: stat error", 20)) {
				sf_finalize(state, -1);
			}
			return;
		}
		sf_handle_stat(state);
		return;

	case SF_PHASE_DONE:
	default:
		return;
	}
}

bool http_send_file_dispatch(http_request_t *request,
                             zend_object *response_obj,
                             http_send_file_request_t *req,
                             void (*on_done)(void *user, int status),
                             void *user)
{
	if (UNEXPECTED(req == NULL || response_obj == NULL || req->path == NULL)) {
		if (req != NULL) http_send_file_request_free(req);
		http_response_synth_error(response_obj, 500, "sendFile: invalid arguments");
		return false;
	}

	const size_t path_len = ZSTR_LEN(req->path);
	if (UNEXPECTED(path_len == 0 || path_len + 4 + 1 >= MAXPATHLEN
	               || ZSTR_VAL(req->path)[0] != '/')) {
		http_send_file_request_free(req);
		http_response_synth_error(response_obj, 500, "sendFile: path must be absolute");
		return false;
	}

	/* Protocol must implement send_static_response. */
	const http_response_stream_ops_t *const ops =
		http_response_get_stream_ops(response_obj);
	if (UNEXPECTED(ops == NULL || ops->send_static_response == NULL)) {
		http_send_file_request_free(req);
		http_response_synth_error(response_obj, 500, "sendFile: protocol does not support sendfile");
		return false;
	}

	sf_state_t *state = ecalloc(1, sizeof(*state));
	state->request      = request;
	state->response_obj = response_obj;
	state->req          = req;
	state->on_done      = on_done;
	state->user         = user;
	state->phase        = SF_PHASE_OPEN;

	state->fs_path_len = path_len;
	state->fs_path     = emalloc(path_len + 1);
	memcpy(state->fs_path, ZSTR_VAL(req->path), path_len + 1);

	state->is_head = (request != NULL && request->method != NULL
	                  && ZSTR_LEN(request->method) == 4
	                  && memcmp(ZSTR_VAL(request->method), "HEAD", 4) == 0);

	/* MIME on the *original* path. The sidecar rewrite below must not
	 * change the Content-Type the client sees. */
	{
		const char *ct = NULL; size_t ct_len = 0;
		(void)http_mime_lookup_by_ext(state->fs_path, state->fs_path_len, &ct, &ct_len);
		state->content_type     = ct;
		state->content_type_len = ct_len;
	}

	(void)sf_try_precompressed(state);

	state->file_io = ZEND_ASYNC_FS_OPEN(state->fs_path, O_RDONLY | O_CLOEXEC, 0);
	if (UNEXPECTED(state->file_io == NULL)) {
		sf_state_free(state);
		http_response_synth_error(response_obj, 500, "sendFile: cannot open file");
		return false;
	}

	sf_cb_t *cb = (sf_cb_t *)ZEND_ASYNC_EVENT_CALLBACK_EX(sf_dispatch, sizeof(sf_cb_t));
	if (UNEXPECTED(cb == NULL)) {
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}
		state->file_io = NULL;
		sf_state_free(state);
		http_response_synth_error(response_obj, 500, "sendFile: callback alloc failed");
		return false;
	}
	cb->base.dispose = sf_cb_dispose;
	cb->state = state;

	if (UNEXPECTED(!state->file_io->event.add_callback(&state->file_io->event, &cb->base))) {
		efree(cb);
		if (state->file_io->event.dispose != NULL) {
			state->file_io->event.dispose(&state->file_io->event);
		}
		state->file_io = NULL;
		sf_state_free(state);
		http_response_synth_error(response_obj, 500, "sendFile: cannot register callback");
		return false;
	}
	state->cb = &cb->base;

	return true;
}
