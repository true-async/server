/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_SERVER_TLS_LAYER_H
#define HTTP_SERVER_TLS_LAYER_H

/*
 * Portability: this layer is platform-independent by construction. It
 * talks to OpenSSL (portable) and to the PHP Zend allocators (portable)
 * and does no socket I/O itself — the connection layer owns the socket
 * via zend_async's abstractions. No direct read(2)/write(2)/close(2)
 * on sockets, no POSIX-only headers, no MSG_NOSIGNAL. The same object
 * file compiles on Linux, macOS, and Windows once OpenSSL >= 3.0 is
 * present (see config.m4 / CMakeLists.txt detection paths).
 */

#include "php.h"

/* Forward declarations exposed unconditionally so non-TLS callers can
 * still type a pointer as `tls_context_t *` (value always NULL in
 * plaintext builds). The full layout is TLS-only. */
typedef struct tls_context_t tls_context_t;
typedef struct tls_session_t tls_session_t;

#ifdef HAVE_OPENSSL

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TLS 1.2 / TLS 1.3 ticket key. Three slots for graceful rotation:
 * encryption uses slot 0 (newest), decryption tries any valid slot,
 * stale clients with an older-key ticket get the ticket re-issued
 * under the current key. */
typedef struct {
    uint8_t name[16];
    uint8_t aes_key[32];
    uint8_t hmac_key[32];
    bool    valid;
} tls_ticket_key_t;

#define TLS_TICKET_KEY_SLOTS 3

/* Per-listener TLS configuration. Owns the SSL_CTX* shared by every
 * session on this listener. Decoupled from http_connection_t so the
 * HTTP/3 path (ngtcp2_crypto_ossl) reuses the same context. */
struct tls_context_t {
    SSL_CTX     *ctx;
    zend_string *cert_path;
    zend_string *key_path;
    bool         ktls_enabled;

    /* Ticket key rotation state. Rotation is lazy-on-encrypt: at
     * ticket-issue time we check the elapsed wall-clock and shift if
     * the period has passed. Avoids a per-listener periodic timer. */
    tls_ticket_key_t ticket_keys[TLS_TICKET_KEY_SLOTS];
    uint64_t         ticket_keys_last_rotation_ns;
};

/* Per-connection TLS session. */
typedef enum {
    TLS_HANDSHAKING,
    TLS_ESTABLISHED,
    TLS_SHUTTING_DOWN,
    TLS_CLOSED
} tls_state_t;

typedef enum {
    TLS_ALPN_NONE,
    TLS_ALPN_HTTP11,
    TLS_ALPN_H2,
    TLS_ALPN_H3
} tls_alpn_t;

/* Which TLS primitive produced the most recent error. TLS_OP_NONE means
 * "no error recorded yet" on a fresh session. Keep the order stable —
 * the tls_op_name() table indexes by this enum. */
typedef enum {
    TLS_OP_NONE = 0,
    TLS_OP_HANDSHAKE,
    TLS_OP_READ,       /* SSL_read_ex */
    TLS_OP_WRITE,      /* SSL_write_ex */
    TLS_OP_COMMIT,     /* BIO_nwrite after tls_reserve_cipher_in */
    TLS_OP_CONSUME,    /* BIO_nread after tls_peek_cipher_out */
    TLS_OP_SHUTDOWN
} tls_op_t;

/* Length of the human-readable reason buffer inside tls_error_info_t.
 * Sized for ERR_error_string_n output (~120 bytes) with headroom. */
#define TLS_ERROR_REASON_CAP 160

/* Most recent per-session error. Populated by tls_layer on any path that
 * returns TLS_IO_ERROR or a commit/consume failure. Read at teardown by
 * the connection layer (php_error_docref(E_NOTICE)) — one structured
 * log per failing connection, no allocation in the hot path. Not cleared
 * on success: callers check `op != TLS_OP_NONE` as the presence flag. */
typedef struct {
    tls_op_t      op;
    int           ssl_err;                      /* SSL_get_error() code; -1 for BIO-side failures */
    unsigned long openssl_err;                  /* ERR_peek_last_error() at capture, or 0 */
    tls_state_t   state_at_fail;                /* session->state at time of failure */
    size_t        bytes_done;                   /* bytes processed in the failing op before error */
    char          reason[TLS_ERROR_REASON_CAP]; /* ERR_error_string_n or fallback; NUL-terminated */
} tls_error_info_t;

struct tls_session_t {
    SSL              *ssl;
    BIO              *internal_bio;   /* side owned by OpenSSL (rbio == wbio) */
    BIO              *network_bio;    /* side we feed/drain */
    tls_state_t       state;
    tls_alpn_t        alpn_selected;
    tls_error_info_t  last_error;     /* zero-initialised; op == TLS_OP_NONE means "no error" */
};

/* I/O step result — mirrors SSL_get_error() distinctions. */
typedef enum {
    TLS_IO_OK,
    TLS_IO_WANT_READ,
    TLS_IO_WANT_WRITE,
    TLS_IO_CLOSED,
    TLS_IO_ERROR
} tls_io_result_t;

/**
 * BIO pair ring-buffer size per direction, in bytes.
 *
 * One TLS record is at most 16 KiB of plaintext + ~325 bytes of
 * framing/overhead (TLS 1.3: 5-byte header + auth tag + content-type).
 * A 17 KiB ring fits exactly one record in steady state; that is the
 * sweet spot — enough to avoid unnecessary fragmentation, small enough
 * to keep per-connection memory predictable.
 */
#define TLS_BIO_RING_SIZE (17 * 1024)

/**
 * Maximum length of an error string written by tls_context_new().
 *
 * Sized for a typical OpenSSL error stack entry (ERR_error_string_n
 * emits ~120 bytes) plus a short prefix identifying our own stage
 * (file load, key mismatch, ...).
 */
#define TLS_ERR_BUF_SIZE 256

/**
 * Create a per-listener TLS context.
 *
 * Loads the PEM certificate chain and private key, verifies they match,
 * and configures the returned SSL_CTX with hardened defaults: TLS 1.2
 * and 1.3 only, no renegotiation, no compression, server-preferred
 * ciphers, stateless tickets, ALPN advertising "http/1.1". If OpenSSL
 * was built with kTLS and the
 * platform exposes it (HAVE_KTLS), SSL_OP_ENABLE_KTLS is set so kernel
 * offload can kick in opportunistically at handshake time.
 *
 * @param cert_path  Path to a PEM certificate chain file.
 * @param key_path   Path to a PEM private key file (matching @p cert_path).
 * @param err_buf    Caller-owned buffer that receives a human-readable
 *                   error description on failure. May be NULL to discard.
 * @param err_cap    Capacity of @p err_buf; ignored if @p err_buf is NULL.
 *                   Recommended size: #TLS_ERR_BUF_SIZE.
 *
 * @return  A newly allocated tls_context_t on success, NULL on failure.
 *          On failure, @p err_buf (if non-NULL) is populated with a
 *          NUL-terminated message suitable for a PHP exception.
 */
tls_context_t *tls_context_new(const char *cert_path,
                               const char *key_path,
                               char *err_buf,
                               size_t err_cap);

#ifdef HAVE_HTTP_SERVER_HTTP3
struct ssl_st; /* forward-decl; avoid pulling openssl/ssl.h here */
/**
 * Mark @p ssl as the QUIC side of the shared SSL_CTX so the ALPN
 * callback picks the QUIC ALPN list (h3) instead of the TCP one
 * (h2, http/1.1). Must be called after SSL_new and before the first
 * SSL_do_handshake / SSL_provide_quic_data. SSL_is_quic() cannot be
 * used here because ngtcp2_crypto_ossl drives the handshake through
 * OpenSSL 3.5's QUIC TLS callbacks, which leave the SSL handle in
 * regular-TLS mode for that predicate.
 */
void tls_layer_mark_ssl_quic(struct ssl_st *ssl);
int  tls_layer_quic_ex_index(void);
#endif

/**
 * Release a TLS context and its underlying SSL_CTX.
 *
 * Safe to call with @p ctx == NULL. The function is idempotent in the
 * sense that it zeroes internal pointers, but the tls_context_t itself
 * must not be reused after free.
 */
void tls_context_free(tls_context_t *ctx);

/* --- Session lifecycle (per connection). --- */
tls_session_t *tls_session_new(tls_context_t *ctx);
void           tls_session_free(tls_session_t *s);

/* --- State-machine primitives. --- */
tls_io_result_t tls_feed_ciphertext(tls_session_t *s, const char *buf, size_t len, size_t *consumed);
tls_io_result_t tls_drain_ciphertext(tls_session_t *s, char *buf, size_t cap, size_t *produced);
tls_io_result_t tls_handshake_step(tls_session_t *s);
tls_io_result_t tls_read_plaintext(tls_session_t *s, char *buf, size_t cap, size_t *produced);
tls_io_result_t tls_write_plaintext(tls_session_t *s, const char *buf, size_t len, size_t *written);
tls_io_result_t tls_shutdown_step(tls_session_t *s);

/* --- Zero-copy BIO access (OpenSSL BIO_n[write|read][0]). -------
 *
 * These four helpers let the caller do socket I/O directly into/out
 * of the BIO pair's internal ring buffer, eliminating one memcpy and
 * one per-connection staging buffer each.
 *
 * Intended usage — read path (network → TLS):
 *
 *     char   *slot;
 *     size_t  space = tls_reserve_cipher_in(session, &slot);
 *     if (space == 0) ... handle backpressure;
 *     ssize_t nread = recv_into(slot, space);   // or async IO read
 *     if (nread > 0) tls_commit_cipher_in(session, (size_t)nread);
 *
 * Write path (TLS → network):
 *
 *     char   *slot;
 *     size_t  avail = tls_peek_cipher_out(session, &slot);
 *     if (avail == 0) break;
 *     size_t sent = write_from(slot, avail);
 *     tls_consume_cipher_out(session, sent);
 *
 * The returned pointer points into the ring buffer and stays valid
 * only until the next operation on the SAME BIO side (another
 * reserve/commit on the write side, or another peek/consume on the
 * read side). OpenSSL never wraps the peeked region across the ring
 * boundary — callers must loop if they want to fill the whole ring
 * in one go.
 *
 * Safety: the session is not thread-safe by design — single-coroutine
 * ownership is the invariant. These functions are fine as long as only
 * one coroutine touches the session at a time.
 */
size_t tls_reserve_cipher_in(tls_session_t *s, char **out_ptr);
/* Returns false if the BIO pair rejects the commit (OOM inside OpenSSL,
 * ring corrupted). On failure the session is marked CLOSED so the next
 * handshake/read/write step propagates TLS_IO_CLOSED. The caller must
 * stop feeding and tear the connection down. */
bool   tls_commit_cipher_in(tls_session_t *s, size_t n);
size_t tls_peek_cipher_out(tls_session_t *s, char **out_ptr);
/* See tls_commit_cipher_in for the failure contract. */
bool   tls_consume_cipher_out(tls_session_t *s, size_t n);

/**
 * True if the just-completed handshake resumed a previous session
 * instead of running the full 1-RTT exchange. Wraps SSL_session_reused.
 * Undefined before handshake completion; callers check only after
 * tls_handshake_step returned TLS_IO_OK.
 */
bool tls_session_was_resumed(const tls_session_t *s);

/**
 * Runtime kTLS probe — returns true if the kernel has taken over TLS
 * record framing for that direction on this session. Only meaningful
 * after the handshake has completed.
 *
 * Engagement requires all of: Linux >= 4.17 (TLS 1.3: 5.2+), the
 * `tls` kernel module loaded, OpenSSL built with enable-ktls, and
 * SSL_OP_ENABLE_KTLS set on the SSL_CTX (we set it unconditionally
 * when HAVE_KTLS is defined). When the kernel refuses, these return
 * false and userspace encryption continues — a silent, safe fallback.
 */
bool tls_session_ktls_tx_active(const tls_session_t *s);
bool tls_session_ktls_rx_active(const tls_session_t *s);

/**
 * Return a pointer to the session's most recent error record. The
 * pointer is valid for the session's lifetime; the struct is mutated
 * in place on each new failure. NULL only if @p s is NULL.
 *
 * Callers check `result->op != TLS_OP_NONE` — TLS_OP_NONE means no
 * error has been recorded on this session yet. The buffer is not
 * cleared on successful operations; stale data from a prior failure
 * remains visible until the next error overwrites it. That is fine for
 * the current consumer (teardown log), but future consumers that want
 * "is the current state errored" should rely on the session's
 * tls_state_t instead.
 *
 * Future: HttpServer::onTlsError(callable $cb) hook that surfaces this
 * struct as a PHP array. Today consumers read it directly at teardown
 * and emit E_NOTICE. Deferred until a real user asks for programmatic
 * access.
 */
const tls_error_info_t *tls_session_last_error(const tls_session_t *s);

/** Human-readable name of a TLS operation. "unknown" for out-of-range op. */
const char *tls_op_name(tls_op_t op);

#endif /* HAVE_OPENSSL */

#endif /* HTTP_SERVER_TLS_LAYER_H */
