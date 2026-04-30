/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Private cross-TU declarations for the HTTP/3 implementation.

  http3_connection.c was a 2571-line monolith and got split per
  audit #8 into four cohesive translation units:

    http3_connection.c    — connection lifecycle (accept/dispatch/free)
                            and the small process-lifetime helpers
                            (timestamps, debug toggle, RNG, OSSL init).
    http3_io.c            — packet/timer machinery (drain_out, timer
                            arm/detach, emit_close, reap, terminal probe).
    http3_callbacks.c     — every ngtcp2 + nghttp3 callback, the two
                            callback dispatch tables, plus the static
                            helpers their bodies share (header table,
                            stream rejection, submit_response, streaming
                            vtable, finalize_request_body, init_h3).
    http3_dispatch.c      — handler-coroutine entry/dispose plus
                            http3_stream_dispatch.

  This header is the contract between those TUs. Public API stays in
  http3_connection.h; this file is private to src/http3/.
*/

#ifndef HTTP3_INTERNAL_H
#define HTTP3_INTERNAL_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif


#include "php.h"
#include "Zend/zend_async_API.h"
#include <ngtcp2/ngtcp2.h>
#include <nghttp3/nghttp3.h>
#include <openssl/ssl.h>

#include "http3_connection.h"

/* Belt-and-braces caps. nghttp3's SETTINGS_MAX_FIELD_SECTION_SIZE
 * defaults to (1<<62)-1 — effectively unlimited. We pin sane defaults
 * so a malformed peer cannot drain memory before flow control catches
 * up. Numbers track the H2 plan's HTTP2_SETTINGS_MAX_HEADER_LIST and
 * HTTP2_MAX_BODY_SIZE so handlers see the same envelope across H1/H2/H3. */
#define HTTP3_MAX_HEADERS_BYTES   (256 * 1024)
#define HTTP3_MAX_BODY_BYTES      (16  * 1024 * 1024)

/* How long our server-chosen SCID should be. 8 bytes matches what nginx
 * and most production stacks use — long enough to be collision-resistant
 * against randomised attack, short enough to save per-packet bytes. */
#define HTTP3_SCID_LEN 8


/* ===== Helpers shared by all H3 TUs (defined in http3_connection.c) ===== */

/* Local timestamp in ngtcp2 format (nanoseconds since some monotonic
 * epoch). ngtcp2 does not care about the epoch as long as we are
 * consistent — zend_hrtime fits. */
ngtcp2_tstamp http3_ts_now(void);

/* Secure random bytes via OpenSSL. Returns true on success. Callers MUST
 * propagate false: a silent zero-fill fallback would produce all-zero
 * SCIDs and all-zero stateless-reset tokens, both of which are
 * security-relevant. */
bool http3_fill_random(uint8_t *buf, size_t len);

/* Cached one-shot init of ngtcp2_crypto_ossl. Idempotent. */
void http3_ensure_ossl_crypto_init(void);

/* ngtcp2 log_printf-compatible bridge into http_log at DEBUG. */
void http3_debug_logger(void *user_data, const char *fmt, ...);

/* Build a sockaddr_storage from the listener's bound (host, port).
 * Required because the reactor doesn't yet expose the actual bound
 * sockname; ngtcp2_path matching is strict so we must reproduce the
 * same value on every read/write. peer_family selects v4/v6. */
int http3_build_listener_local(const http3_listener_t *l,
                               int peer_family,
                               struct sockaddr_storage *out,
                               socklen_t *out_len);


/* ===== Packet/timer machinery (defined in http3_io.c) ===== */

/* Drive the ngtcp2/nghttp3 send loop until the peer is up to date.
 * Coalesces multiple QUIC packets into one sendmsg+UDP_SEGMENT (GSO)
 * batch where MTU + ECN allow. */
void http3_connection_drain_out(http3_connection_t *c);

/* (Re)arm the ngtcp2 retransmission/PTO/idle timer with the next expiry
 * from ngtcp2_conn_get_expiry. No-op if ngtcp2 has nothing scheduled. */
void http3_connection_arm_timer(http3_connection_t *c);

/* Cancel + free the timer slot. Safe to call when no timer is armed. */
void http3_connection_detach_timer(http3_connection_t *c);

/* Synchronously emit one CONNECTION_CLOSE datagram. No-op if already
 * sent or if the connection is in the draining period (peer-initiated). */
void http3_connection_emit_close(http3_connection_t *c);

/* Post-IO terminal probe: if ngtcp2 transitioned into closing/draining,
 * emit_close (no-op if peer-initiated) and reap. Returns true iff the
 * caller must NOT touch `c` again. */
bool http3_connection_check_terminal(http3_connection_t *c);


/* ===== nghttp3/ngtcp2 callback tables (defined in http3_callbacks.c) ===== */

/* nghttp3 application callbacks: HEADERS/DATA/end-stream/close, plus
 * acked_stream_data for the chunk-queue release schedule. */
extern const nghttp3_callbacks HTTP3_NGHTTP3_CALLBACKS;

/* ngtcp2 transport callbacks: crypto bridge, stream-data forwarding to
 * nghttp3, flow-control wakeups, stream lifecycle bridge. */
extern const ngtcp2_callbacks HTTP3_NGTCP2_CALLBACKS;

/* Allocate the per-connection nghttp3_conn + open the three server
 * unidirectional streams (control, QPACK encoder/decoder). Bound to
 * ngtcp2 streams via nghttp3_conn_bind_*_stream. Returns true on
 * success; on failure rolls back any partial state. Defined in
 * http3_callbacks.c next to the nghttp3 callback table. */
bool http3_connection_init_h3(http3_connection_t *c);


/* ===== Stream dispatch (defined in http3_dispatch.c) ===== */

/* Full user-handler dispatch. Builds (HttpRequest,
 * HttpResponse) zvals, spawns the per-stream handler coroutine in the
 * server scope. Idempotent — short-circuits on dispatched/rejected
 * streams. Public to http3_callbacks.c (h3_end_headers_cb /
 * h3_end_stream_cb fall-through path). */
typedef struct _http3_stream_s http3_stream_t;  /* defined in include/http3/http3_stream.h */
void http3_stream_dispatch(http3_connection_t *c, http3_stream_t *s);


/* ===== Symbols defined in http3_callbacks.c, used from http3_dispatch.c ===== */

#include "php_http_server.h"  /* http_response_stream_ops_t */

/* Streaming response vtable (HttpResponse::send → chunk_queue). */
extern const http_response_stream_ops_t h3_stream_ops;

/* Buffered REST response commit. The dispose path of the handler
 * coroutine (in http3_dispatch.c) calls this when nothing was streamed
 * via $res->send() — submit_response with the single-slice data_reader. */
bool http3_stream_submit_response(http3_connection_t *c,
                                  http3_stream_t *s,
                                  bool streaming);



#endif /* HTTP3_INTERNAL_H */
