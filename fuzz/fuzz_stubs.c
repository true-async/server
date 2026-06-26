/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * Link-time stubs for extension-level symbols referenced by our
 * parser/session TUs but irrelevant to fuzz harnesses (which exercise
 * raw input paths, not user-PHP dispatch / live extensions).
 *
 * Matches the pattern in tests/unit/common/multipart_stubs.c — weak
 * defaults that real unit tests or the production extension can
 * override.
 */

#include "php.h"
#include "http1/http_parser.h"   /* http_request_t layout + HTTP_HEADERS_INITIAL_SIZE */

/* Extension class entries (normally populated at MINIT). */
zend_class_entry *http_exception_ce __attribute__((weak)) = NULL;

/* http_request_init_headers lives in http_request.c (the PHP-object TU, not
 * linked into the fuzz harness). http_parser.c / http2_session.c call it to
 * lazily allocate req->headers before storing parsed headers, so a no-op
 * would leave the HT NULL and crash the path under test. Fuzz requests are
 * always ZMM (non-persistent), so the real non-persistent init is correct. */
__attribute__((weak)) void http_request_init_headers(http_request_t *req)
{
    if (req->headers != NULL) {
        return;
    }

    ALLOC_HASHTABLE(req->headers);
    zend_hash_init(req->headers, HTTP_HEADERS_INITIAL_SIZE, NULL, ZVAL_PTR_DTOR, 0);
}

/* Server-level telemetry hooks invoked by h2 session/strategy TUs.
 * All NULL-safe in production; here they're no-ops since fuzz has
 * no server object to count against. */
__attribute__((weak)) void http_server_on_h2_stream_opened(void *server)  { (void)server; }
__attribute__((weak)) void http_server_on_h2_stream_closed(void *server)  { (void)server; }
__attribute__((weak)) void http_server_on_h2_ping_rtt(void *server, uint64_t rtt_ns) { (void)server; (void)rtt_ns; }
__attribute__((weak)) void http_server_on_h2_stream_reset_by_peer(void *server) { (void)server; }
__attribute__((weak)) void http_server_on_h2_goaway_recv(void *server) { (void)server; }
__attribute__((weak)) void http_server_on_h2_data_recv(void *server, size_t bytes) { (void)server; (void)bytes; }
__attribute__((weak)) void http_server_on_h2_data_sent(void *server, size_t bytes) { (void)server; (void)bytes; }
__attribute__((weak)) void http_server_on_parse_error(void *server, int status_code) { (void)server; (void)status_code; }

/* Header interning lookup — return NULL so the parser falls back to
 * zend_string_init. Same semantics, just no caching. */
__attribute__((weak)) zend_string *http_known_header_lookup(const char *name, size_t len)
{
    (void)name; (void)len;
    return NULL;
}

/* Logging is irrelevant for fuzz harnesses; drop. Forward-declared
 * to avoid pulling http_log.h (which transitively pulls async event
 * machinery). */
struct http_log_state;
struct http_log_attr;
__attribute__((weak)) void http_log_emitf(struct http_log_state *state, int sev,
                                          const struct http_log_attr *attrs, size_t n,
                                          const char *tmpl, ...)
{
    (void)state; (void)sev; (void)attrs; (void)n; (void)tmpl;
}

/* Trace-context (W3C traceparent / B3) parsing is logging metadata;
 * fuzz harnesses don't need it. */
struct http_request_t;
__attribute__((weak)) void http_request_parse_trace_context(struct http_request_t *req)
{
    (void)req;
}

/* http2_session_emit lives in http2_strategy.c (not linked into the
 * fuzz harness). The buffered data_provider's write_event subscriber
 * — added to wake setBody >64K after WINDOW_UPDATE — references it,
 * but parser-only fuzzing never actually fires that callback (no live
 * connection, no real emit path). Stub to satisfy the linker. */
struct http2_session_t;
__attribute__((weak)) void http2_session_emit(struct http2_session_t *session)
{
    (void)session;
}

/* http_server module globals id — normally defined by
 * ZEND_DECLARE_MODULE_GLOBALS in src/http_server.c, which the fuzz
 * link does not pull in. ZTS-only (we always build PHP --enable-zts
 * for fuzzing). The H2 session uses HTTP_SERVER_G() macro which
 * dereferences the TSRM slot keyed by this id; the slot stays
 * uninitialised and reads return zero — fine for parser-only fuzzing. */
#ifdef ZTS
__attribute__((weak)) int http_server_globals_id = 0;
#endif

/* async_plain_event_new — invoked by http_body_stream_push for the
 * body_data_event wake. Fuzz harnesses don't drive a real coroutine
 * reader, so returning NULL is acceptable: http_body_stream_push then
 * marks body_error and bails out, exercising the error path. */
struct zend_async_event_s;
__attribute__((weak)) struct zend_async_event_s *async_plain_event_new(void)
{
    return NULL;
}

/* http_body_stream_pop calls these on h2 streaming bodies to grant the
 * peer credit (commit c812184: per-stream INITIAL_WINDOW=64K + flow-
 * control backpressure). Fuzz harnesses don't drive a real h2 session,
 * so no-op is safe — the parser path under test is identical. */
struct nghttp2_session;
struct http2_session_t;
__attribute__((weak)) int nghttp2_session_consume(struct nghttp2_session *session,
                                                  int32_t stream_id, size_t size)
{
    (void)session; (void)stream_id; (void)size;
    return 0;
}

__attribute__((weak)) void h2_session_schedule_emit(struct http2_session_t *session)
{
    (void)session;
}

/* h2_static_account_debit lives in http2_static_response.c (not linked
 * into the fuzz harness). The inline release wrappers in
 * include/http2/http2_stream.h call it from drain paths in http2_session.c
 * and http2_stream.c via stream->static_tracks_chunks gating — fuzz
 * inputs never set that flag (no static FSM), so the symbol is never
 * actually invoked at run-time. Stub satisfies the linker only. */
struct _http_connection_t;
__attribute__((weak)) void h2_static_account_debit(struct _http_connection_t *conn,
                                                   size_t n)
{
    (void)conn; (void)n;
}
