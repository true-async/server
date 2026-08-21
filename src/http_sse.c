/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Server-Sent Events (text/event-stream).
 *
 * SSE is not a separate protocol — it is a Content-Type convention plus
 * the small line-oriented framing defined by WHATWG §9.2, layered on top
 * of the existing HttpResponse::write() streaming pipeline (HTTP/1 chunked,
 * HTTP/2 + HTTP/3 DATA frames). These helpers only (1) set the canonical
 * headers so a handler can't ship a broken stream behind nginx/a CDN and
 * (2) format event records correctly so handlers don't reinvent framing.
 *
 * Wire commit is lazy: the headers are set and the response is locked into
 * streaming mode here, but the actual HEADERS frame / status line is
 * emitted by the protocol stream_ops on the first append_chunk — exactly
 * the same path the first write() drives. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "zend_smart_str.h"
#include "php_http_server.h"
#include "http_response_internal.h"

#ifdef HAVE_HTTP_COMPRESSION
#include "compression/http_compression_response.h"
#endif

/* Which call finished the response. The two are refused alike, and a handler
 * told only that the response is closed has to guess which of its own calls
 * did it. */
static const char *sse_finished_by(const http_response_object *response)
{
	return response->aborted ? "abort()" : "end()";
}

/* Events are delimited by a blank line, fields by a single LF. WHATWG
 * accepts CR / CRLF on input but we always emit LF. CR / LF inside a
 * single-line field (event / id) is a framing-injection bug — reject it. */
static bool sse_has_newline(const zend_string *s)
{
	if (s == NULL) {
		return false;
	}

	const char *const p = ZSTR_VAL(s);
	const size_t len = ZSTR_LEN(s);

	for (size_t i = 0; i < len; i++) {
		if (p[i] == '\n' || p[i] == '\r') {
			return true;
		}
	}

	return false;
}

/* Set one header by literal lowercase name, replacing any prior value.
 * Stores a flat IS_STRING — the canonical single-value shape every wire
 * formatter already understands (see add_header_value in http_response.c). */
static void sse_set_header(HashTable *headers, const char *name, size_t name_len, const char *value,
						   size_t value_len)
{
	zend_string *const key = zend_string_init(name, name_len, 0);
	zval v;
	ZVAL_STRINGL(&v, value, value_len);
	zend_hash_update(headers, key, &v);
	zend_string_release(key);
}

/* Reject sseStart() over a response that already carries a non-SSE
 * Content-Type — that is a programming bug, not something to paper over.
 * A leading "text/event-stream" (with optional parameters) is allowed. */
static bool sse_content_type_conflicts(const HashTable *headers)
{
	zval *const ct = zend_hash_str_find(headers, "content-type", sizeof("content-type") - 1);
	if (ct == NULL) {
		return false;
	}

	const char *val = NULL;
	size_t val_len = 0;
	if (Z_TYPE_P(ct) == IS_STRING) {
		val = Z_STRVAL_P(ct);
		val_len = Z_STRLEN_P(ct);
	} else if (Z_TYPE_P(ct) == IS_ARRAY) {
		zval *const first = zend_hash_index_find(Z_ARRVAL_P(ct), 0);
		if (first != NULL && Z_TYPE_P(first) == IS_STRING) {
			val = Z_STRVAL_P(first);
			val_len = Z_STRLEN_P(first);
		}
	}

	static const char expected[] = "text/event-stream";
	static const size_t expected_len = sizeof(expected) - 1;

	if (val != NULL && val_len >= expected_len &&
		zend_binary_strcasecmp(val, expected_len, expected, expected_len) == 0) {
		return false;
	}

	return val != NULL;
}

/* Idempotent SSE init: validate Content-Type, set the three canonical
 * headers, mark the response non-compressible, and switch it into
 * streaming mode at the PHP boundary. Emits nothing on the wire — the
 * first append_chunk drives the protocol header commit. Returns false on
 * error with an exception already thrown. */
static bool sse_ensure_started(http_response_object *response)
{
	if (response->streaming) {
		/* Already streaming via write() (or another non-SSE path) — emitting
		 * SSE framing now would ship event records without the event-stream
		 * headers, and possibly through write()'s gzip wrapper. Reject the
		 * misuse instead of silently corrupting the stream. */
		if (!response->sse_mode) {
			zend_throw_exception(http_server_runtime_exception_ce,
								 "Response is already streaming via write() — cannot switch to SSE", 0);
			return false;
		}

		return true;
	}

	if (response->closed) {
		zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
								"Cannot start SSE after %s", sse_finished_by(response));
		return false;
	}

	/* RFC 9112 §6.3 rule 1: a 1xx, a 204 and a 304 end at the blank line, so an
	 * event stream on one has no body to put its records in and every event
	 * would be refused. Said here, at the gate the whole dialect passes,
	 * while the response is uncommitted and can still become a status. */
	if (!response_status_carries_body(response->status_code)) {
		zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
								"Cannot start SSE on status %d — it carries no body",
								response->status_code);
		return false;
	}

	if (response->send_file_req != NULL) {
		zend_throw_exception(http_server_runtime_exception_ce,
							 "Response is sealed by sendFile() — cannot switch to SSE", 0);
		return false;
	}

	if (UNEXPECTED(response->stream_ops == NULL)) {
		zend_throw_exception(http_server_runtime_exception_ce,
							 "SSE requires a streaming-capable response (no stream ops — "
							 "response is detached from a connection)",
							 0);
		return false;
	}

	if (sse_content_type_conflicts(response->headers)) {
		zend_throw_exception(http_server_invalid_argument_exception_ce,
							 "Response already has a non-SSE Content-Type — remove it before "
							 "switching to SSE",
							 0);
		return false;
	}

	sse_set_header(response->headers, "content-type", sizeof("content-type") - 1,
				   "text/event-stream", sizeof("text/event-stream") - 1);
	sse_set_header(response->headers, "cache-control", sizeof("cache-control") - 1,
				   "no-cache, no-transform", sizeof("no-cache, no-transform") - 1);
	/* nginx-specific: disables proxy_buffering for this response. Harmless
	 * on protocols / proxies that don't recognise it. */
	sse_set_header(response->headers, "x-accel-buffering", sizeof("x-accel-buffering") - 1, "no",
				   sizeof("no") - 1);

#ifdef HAVE_HTTP_COMPRESSION
	/* A buffering gzip stream defeats real-time delivery — never compress
	 * an event stream. SSE dispatches through the raw stream_ops (not the
	 * write() wrapper), but mark it explicitly so intent is unambiguous. */
	http_compression_mark_no_compression(&response->std);
#endif

	response->streaming = true;
	response->sse_mode = true;
	response->committed = true;
	response->headers_sent = true;
	return true;
}

/* Append one "data: <line>\n" per line of `value`, splitting on
 * LF / CR / CRLF (WHATWG §9.2). A terminator at the very end yields a
 * trailing empty data line so the consumer reconstructs the final LF. */
static void sse_append_data(smart_str *out, const char *p, size_t len)
{
	size_t i = 0;
	while (true) {
		const size_t start = i;
		while (i < len && p[i] != '\n' && p[i] != '\r') {
			i++;
		}

		smart_str_appendl(out, "data: ", sizeof("data: ") - 1);
		smart_str_appendl(out, p + start, i - start);
		smart_str_appendc(out, '\n');

		if (i >= len) {
			break;
		}

		if (p[i] == '\r' && i + 1 < len && p[i + 1] == '\n') {
			i += 2;
		} else {
			i++;
		}

		if (i >= len) {
			smart_str_appendl(out, "data: \n", sizeof("data: \n") - 1);
			break;
		}
	}
}

/* Append "<field>: <value>\n". Caller guarantees `value` has no CR / LF. */
static void sse_append_field(smart_str *out, const char *field, size_t field_len, const char *value,
							 size_t value_len)
{
	smart_str_appendl(out, field, field_len);
	smart_str_appendl(out, ": ", 2);
	if (value_len > 0) {
		smart_str_appendl(out, value, value_len);
	}
	smart_str_appendc(out, '\n');
}

/* Push a finalised event payload through the installed stream ops.
 * append_chunk takes ownership of the payload ref (so we never release it)
 * and, unless @p nonblocking, suspends the handler under backpressure on
 * H2/H3. Mirrors write(): a dead stream surfaces as a 499 the handler may
 * catch. Returns the append result, which a non-blocking caller reads to tell
 * a refusal from a delivery. */
static int sse_dispatch(http_response_object *response, zend_string *payload, const bool nonblocking)
{
	/* A HEAD response carries the headers a GET would and none of its body
	 * (RFC 9110 §9.3.2). The record is built so the handler runs the same code
	 * for both methods, then dropped — the answer write() gives a HEAD. */
	if (response->is_head) {
		zend_string_release(payload);
		return HTTP_STREAM_APPEND_OK;
	}

	const int rc = response->stream_ops->append_chunk(response->stream_ctx, payload, nonblocking);

	if (rc == HTTP_STREAM_APPEND_STREAM_DEAD) {
		zend_throw_exception_ex(http_exception_ce, 499, "stream closed by peer");
	}

	return rc;
}

/* Reject what a record cannot carry: CR or LF in a single-line field would
 * inject further fields or end the record early, a NUL in `id` makes the
 * parser ignore the field (WHATWG §9.2), and a negative retry is not a delay.
 * @p method names the caller in the exception message. Throws and returns
 * false. */
static bool sse_event_fields_valid(zend_string *event, zend_string *id, const zend_long retry,
								   const bool retry_is_null, const char *method)
{
	if (sse_has_newline(event)) {
		zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
								"%s(): $event must not contain CR or LF", method);
		return false;
	}

	if (sse_has_newline(id)) {
		zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
								"%s(): $id must not contain CR or LF", method);
		return false;
	}

	if (id != NULL && memchr(ZSTR_VAL(id), '\0', ZSTR_LEN(id)) != NULL) {
		zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
								"%s(): $id must not contain NUL bytes", method);
		return false;
	}

	if (!retry_is_null && retry < 0) {
		zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
								"%s(): $retry must be a non-negative integer", method);
		return false;
	}

	return true;
}

/* Build one event record from the four fields. Field order is id, event,
 * retry, data — a spec parser is order-agnostic, and this is what every
 * implementation emits. Returns a fresh reference for append_chunk. */
static zend_string *sse_event_record(zend_string *data, zend_string *event, zend_string *id,
									 const zend_long retry, const bool retry_is_null)
{
	smart_str payload = {0};

	if (id != NULL) {
		sse_append_field(&payload, "id", 2, ZSTR_VAL(id), ZSTR_LEN(id));
	}

	if (event != NULL) {
		sse_append_field(&payload, "event", 5, ZSTR_VAL(event), ZSTR_LEN(event));
	}

	if (!retry_is_null) {
		char buf[32];
		const int n = snprintf(buf, sizeof(buf), ZEND_LONG_FMT, retry);
		if (n > 0) {
			sse_append_field(&payload, "retry", 5, buf, (size_t) n);
		}
	}

	if (data != NULL) {
		sse_append_data(&payload, ZSTR_VAL(data), ZSTR_LEN(data));
	}

	smart_str_appendc(&payload, '\n');
	smart_str_0(&payload);

	return payload.s;
}

/* {{{ proto HttpResponse::sseStart(): static */
ZEND_METHOD(TrueAsync_HttpResponse, sseStart)
{
	ZEND_PARSE_PARAMETERS_NONE();

	http_response_object *const response = Z_HTTP_RESPONSE_P(ZEND_THIS);

	if (response->streaming) {
		zend_throw_exception(http_server_runtime_exception_ce,
							 "sseStart(): response is already in streaming mode", 0);
		return;
	}

	if (!sse_ensure_started(response)) {
		return;
	}

	RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::sseEvent(?string $data = null, ?string $event = null,
 *                                  ?string $id = null, ?int $retry = null): static */
ZEND_METHOD(TrueAsync_HttpResponse, sseEvent)
{
	zend_string *data = NULL;
	zend_string *event = NULL;
	zend_string *id = NULL;
	zend_long retry = 0;
	bool retry_is_null = true;

	ZEND_PARSE_PARAMETERS_START(0, 4)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR_OR_NULL(data)
	Z_PARAM_STR_OR_NULL(event)
	Z_PARAM_STR_OR_NULL(id)
	Z_PARAM_LONG_OR_NULL(retry, retry_is_null)
	ZEND_PARSE_PARAMETERS_END();

	http_response_object *const response = Z_HTTP_RESPONSE_P(ZEND_THIS);

	if (response->closed) {
		zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
								"Cannot sseEvent() after %s", sse_finished_by(response));
		return;
	}

	if (!sse_event_fields_valid(event, id, retry, retry_is_null, "sseEvent")) {
		return;
	}

	/* All arguments unset — nothing to dispatch, don't start the stream. */
	if (data == NULL && event == NULL && id == NULL && retry_is_null) {
		RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
	}

	if (!sse_ensure_started(response)) {
		return;
	}

	sse_dispatch(response, sse_event_record(data, event, id, retry, retry_is_null), false);
	if (EG(exception)) {
		return;
	}

	RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::trySseEvent(?string $data = null, ?string $event = null,
 *                                     ?string $id = null, ?int $retry = null): bool */
ZEND_METHOD(TrueAsync_HttpResponse, trySseEvent)
{
	zend_string *data = NULL;
	zend_string *event = NULL;
	zend_string *id = NULL;
	zend_long retry = 0;
	bool retry_is_null = true;

	ZEND_PARSE_PARAMETERS_START(0, 4)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR_OR_NULL(data)
	Z_PARAM_STR_OR_NULL(event)
	Z_PARAM_STR_OR_NULL(id)
	Z_PARAM_LONG_OR_NULL(retry, retry_is_null)
	ZEND_PARSE_PARAMETERS_END();

	http_response_object *const response = Z_HTTP_RESPONSE_P(ZEND_THIS);

	if (response->closed) {
		zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
								"Cannot trySseEvent() after %s", sse_finished_by(response));
		return;
	}

	if (!sse_event_fields_valid(event, id, retry, retry_is_null, "trySseEvent")) {
		return;
	}

	/* Nothing to dispatch is not a refusal: the queue was never asked. */
	if (data == NULL && event == NULL && id == NULL && retry_is_null) {
		RETURN_TRUE;
	}

	/* Dead peer before the stream starts, for two reasons: false then carries
	 * one meaning only — the queue is full — and the 499 leaves the response
	 * uncommitted, so a handler catching it can still answer with a status. */
	if (response->stream_ops != NULL && response->stream_ops->is_alive != NULL
		&& !response->stream_ops->is_alive(response->stream_ctx)) {
		zend_throw_exception_ex(http_exception_ce, 499, "stream closed by peer");
		return;
	}

	if (!sse_ensure_started(response)) {
		return;
	}

	const int rc = sse_dispatch(response, sse_event_record(data, event, id, retry, retry_is_null), true);

	if (EG(exception)) {
		return;
	}

	RETURN_BOOL(rc != HTTP_STREAM_APPEND_BACKPRESSURE);
}
/* }}} */

/* {{{ proto HttpResponse::sseComment(string $text = ""): static */
ZEND_METHOD(TrueAsync_HttpResponse, sseComment)
{
	zend_string *text = NULL;

	ZEND_PARSE_PARAMETERS_START(0, 1)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR(text)
	ZEND_PARSE_PARAMETERS_END();

	http_response_object *const response = Z_HTTP_RESPONSE_P(ZEND_THIS);

	if (response->closed) {
		zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
								"Cannot sseComment() after %s", sse_finished_by(response));
		return;
	}

	if (sse_has_newline(text)) {
		zend_throw_exception(http_server_invalid_argument_exception_ce,
							 "sseComment(): $text must not contain CR or LF", 0);
		return;
	}

	if (!sse_ensure_started(response)) {
		return;
	}

	smart_str payload = {0};
	smart_str_appendc(&payload, ':');
	if (text != NULL && ZSTR_LEN(text) > 0) {
		smart_str_appendc(&payload, ' ');
		smart_str_append(&payload, text);
	}
	smart_str_appendl(&payload, "\n\n", 2);
	smart_str_0(&payload);

	sse_dispatch(response, payload.s, false);
	if (EG(exception)) {
		return;
	}

	RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpResponse::sseRetry(int $milliseconds): static */
ZEND_METHOD(TrueAsync_HttpResponse, sseRetry)
{
	zend_long milliseconds;

	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG(milliseconds)
	ZEND_PARSE_PARAMETERS_END();

	http_response_object *const response = Z_HTTP_RESPONSE_P(ZEND_THIS);

	if (response->closed) {
		zend_throw_exception_ex(http_server_runtime_exception_ce, 0,
								"Cannot sseRetry() after %s", sse_finished_by(response));
		return;
	}

	if (milliseconds < 0) {
		zend_throw_exception(http_server_invalid_argument_exception_ce,
							 "sseRetry(): $milliseconds must be a non-negative integer", 0);
		return;
	}

	if (!sse_ensure_started(response)) {
		return;
	}

	char buf[32];
	const int n = snprintf(buf, sizeof(buf), ZEND_LONG_FMT, milliseconds);

	smart_str payload = {0};
	sse_append_field(&payload, "retry", 5, buf, n > 0 ? (size_t)n : 0);
	smart_str_appendc(&payload, '\n');
	smart_str_0(&payload);

	sse_dispatch(response, payload.s, false);
	if (EG(exception)) {
		return;
	}

	RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */
