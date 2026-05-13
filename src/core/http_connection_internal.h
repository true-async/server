/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Private cross-TU contract between http_connection.c (plaintext path
  + lifecycle) and http_connection_tls.c (TLS coroutine + ring writer).

  Public API stays in http_connection.h. The split keeps the plaintext
  read/dispatch path and the TLS BIO machinery independently navigable
  while letting both TUs reach a small shared helper layer (per-await
  IO event, async req await, send_raw, parse-error cancel, protocol
  detect).
*/

#ifndef HTTP_CONNECTION_INTERNAL_H
#define HTTP_CONNECTION_INTERNAL_H

#include "php.h"
#include "Zend/zend_async_API.h"
#include "http_connection.h"
#include "http1/http_parser.h"

#include <limits.h>
#include <stdint.h>

/* Convert seconds (uint32_t) to milliseconds (int), clamping to INT_MAX to
 * avoid signed overflow for large configured timeouts. Inline so both TUs
 * fold it into the call site. */
static inline int seconds_to_ms_clamped(const uint32_t seconds)
{
    const uint64_t ms = (uint64_t)seconds * 1000u;
    return ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms;
}

/* ===== Per-await IO event (defined in http_connection.c) =====
 *
 * Wraps a single ZEND_ASYNC_IO_READ/WRITE submission with a private
 * waker event so an unrelated completion on the shared io->event
 * doesn't falsely wake the suspended coroutine. Used by the handler
 * coroutine (response sends) and by the TLS producer flusher's
 * ciphertext drain. Not used by the TLS read FSM, which lives in
 * event-loop callback context and never suspends. */
typedef enum {
    HTTP_IO_REQ_READ,
    HTTP_IO_REQ_WRITE,
} http_io_req_op_t;

/* Block on a submitted async-io req. Returns true iff the req completed
 * before the timeout fired and no exception was raised. timeout_ms == 0
 * disables the timeout (used for handshake bring-up). */
struct http_log_state;
bool async_io_req_await(zend_async_io_req_t *req, zend_async_io_t *io,
                        uint32_t timeout_ms, http_io_req_op_t op,
                        struct http_log_state *log_state);

/* ===== Plaintext send + protocol detect + parse-error cancel
 *       (defined in http_connection.c, used by both TUs) ===== */

/* Loop-write @p len bytes through ZEND_ASYNC_IO_WRITE. The TLS write path
 * calls this to ship ciphertext after SSL_write produces it. */
bool http_connection_send_raw(http_connection_t *conn,
                              const char *data, size_t len);

/* Sniff the protocol from the first bytes in conn->read_buffer and
 * install conn->strategy. Called from the post-handshake feed step
 * on both the plaintext callback and the TLS read FSM. */
bool detect_and_assign_protocol(http_connection_t *conn);

/* Cancel an in-flight handler coroutine with HttpException(parse_error)
 * so it sees the failure at its next suspend point. Both the plaintext
 * read callback and the TLS read FSM call this when a parser limit
 * trips after on_headers_complete already dispatched. */
void http_connection_cancel_handler_for_parse_error(http_connection_t *conn);

/* on_request_ready callback wired into the protocol strategy. The TLS
 * FSM's ALPN fast-path installs the strategy directly (skipping the
 * byte-level detect) and pre-binds this callback. */
void http_connection_on_request_ready(http_connection_t *conn, http_request_t *req);

/* Shared cleanup tail used by both http_handler_coroutine_dispose and
 * the static-handler's hard-zero callback chain (issue #13). Releases
 * per-request state (zvals + ctx), updates conn flags, applies the
 * keep-alive decision, and either resumes the TLS FSM, drains a
 * pipelined request out of conn->read_buffer, or destroys the conn.
 *
 * `should_continue` is the keep-alive verdict the caller has already
 * computed from the response state (true == reuse the conn for the
 * next request; false == close after this dispatch). */
void http_request_finalize(http_connection_t *conn, http1_request_ctx_t *ctx,
                           bool should_continue);

/* PHP-handler coroutine entry + dispose. Exposed so the static-handler
 * on_missing:Next rollback (issue #13 / TODO §5c) can hand a half-
 * dispatched ctx over to a freshly-spawned coroutine without going
 * back through the read path's dispatch entrypoint. */
void http_handler_coroutine_entry(void);
void http_handler_coroutine_dispose(zend_coroutine_t *coroutine);

#ifdef HAVE_OPENSSL
/* ===== TLS path entry points (defined in http_connection_tls.c) ===== */

/* Push @p len plaintext bytes through the encrypted channel. MUST be
 * called from a coroutine — waits on tls_space_event for ring room,
 * BIO_writes atomically, kicks the drain. Returns false only on a
 * sticky TLS write error. */
bool tls_push(http_connection_t *conn, const char *data, size_t len);

/* Scheduler-side TLS drain: SSL_writes plaintext from the BIO ring
 * into the cipher BIO and submits one ciphertext span via WRITE_EX.
 * Never yields, safe to call from any non-coroutine context. Exposed
 * so the h2 emit pump can flush after BIO_write'ing fresh h2 frames. */
bool tls_drain(http_connection_t *conn);

/* Arm the TLS read FSM on a freshly created connection. Submits the
 * first one-shot read into the cipher BIO, attaches the persistent
 * read callback to io->event, and returns. The first ciphertext chunk
 * fired by libuv will drive the handshake state machine to completion.
 * On submission failure the connection is destroyed and the function
 * returns false. */
bool http_connection_tls_arm_read(http_connection_t *conn);

/* Re-enter the TLS FSM after a handler coroutine has finished its
 * dispose path. Called from http_handler_coroutine_dispose when
 * conn->tls != NULL. The FSM resumes either by feeding a pipelined
 * request that arrived during dispatch, by re-arming the cipher read,
 * or by transitioning to graceful close + destroy on a non-keep-alive
 * response. */
void http_connection_tls_resume_after_handler(http_connection_t *conn);

/* True iff a non-blocking FSM async send is currently in flight on
 * this connection. http_connection_destroy uses this to defer
 * teardown until the libuv write completes — the heap buffer is
 * still owned by the in-flight uv_write and only the FSM completion
 * callback can safely free it. */
bool http_connection_tls_fsm_send_in_flight(const http_connection_t *conn);

/* Encrypt and queue a small plaintext message from FSM (read callback)
 * context. Used by http_connection_emit_parse_error when a parse error
 * trips before any handler is dispatched. Caller's @p len must fit in
 * one TLS record (~16 KiB) so a single SSL_write succeeds; the parse-
 * error responder ships under 256 bytes. Returns true on full encrypt
 * + async-send kick. */
bool http_connection_tls_fsm_send_plaintext_atomic(http_connection_t *conn,
                                                   const char *data,
                                                   size_t len);

/* Dispose tls_space_event and wake every coroutine still parked on
 * it. Called from http_connection_destroy before BIOs go away. */
void tls_release_waiters(http_connection_t *conn);
#endif /* HAVE_OPENSSL */

#endif /* HTTP_CONNECTION_INTERNAL_H */
