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

/* Extension class entries (normally populated at MINIT). */
zend_class_entry *http_exception_ce __attribute__((weak)) = NULL;

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

/* http_server module globals id — normally defined by
 * ZEND_DECLARE_MODULE_GLOBALS in src/http_server.c, which the fuzz
 * link does not pull in. ZTS-only (we always build PHP --enable-zts
 * for fuzzing). The H2 session uses HTTP_SERVER_G() macro which
 * dereferences the TSRM slot keyed by this id; the slot stays
 * uninitialised and reads return zero — fine for parser-only fuzzing. */
#ifdef ZTS
__attribute__((weak)) int http_server_globals_id = 0;
#endif
