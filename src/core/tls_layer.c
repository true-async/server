/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/**
 * @file tls_layer.c
 * @brief Per-listener TLS context and per-connection TLS session.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "tls_layer.h"

#ifdef HAVE_OPENSSL

#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include "Zend/zend_hrtime.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#if OPENSSL_VERSION_NUMBER < 0x30000000L
# error "http_server TLS requires OpenSSL >= 3.0"
#endif

/*
 * TLS 1.3 ciphersuites and TLS 1.2 cipher list — Mozilla "intermediate"
 * compatibility profile (Jan 2026 snapshot). Kept as file-scope constants
 * so the strings live in .rodata and are shared across contexts.
 */
static const char tls_ciphersuites_v13[] =
    "TLS_AES_256_GCM_SHA384:"
    "TLS_CHACHA20_POLY1305_SHA256:"
    "TLS_AES_128_GCM_SHA256";

static const char tls_cipher_list_v12[] =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";

/*
 * ALPN wire format: a sequence of length-prefixed byte strings.
 *
 * Two distinct lists: one for the TCP/TLS path (h2 + http/1.1), one for
 * the QUIC path (h3-only). The SSL_CTX is shared between both paths, so
 * the select callback picks the right list at runtime based on whether
 * the SSL object is a QUIC one (SSL_is_quic). A merged list would
 * technically work for cooperative clients, but a misbehaving TCP
 * client that advertised "h3" first would have us ALPN-select h3 on a
 * TCP session where no H3 strategy exists — a configuration cliff we
 * avoid by pinning the advertised set to the transport.
 *
 * Order within each list is server preference (RFC 7301 §3.2, via
 * SSL_select_next_proto's server-preference flavour).
 *
 * When HAVE_HTTP2 is off at build time, the "h2" entry is dropped so
 * a client that only offers "h2" gets a clean ALPN alert instead of
 * a session that negotiates h2 but has no parser for it. */
static const uint8_t tls_alpn_tcp_list[] = {
#ifdef HAVE_HTTP2
    2, 'h', '2',
#endif
    8, 'h', 't', 't', 'p', '/', '1', '.', '1'
};

#ifdef HAVE_HTTP_SERVER_HTTP3
/* QUIC ALPN identifier per RFC 9114. HTTP/3 only — no downgrade to h2
 * over QUIC (there is no such protocol) or http/1.1 over QUIC. */
static const uint8_t tls_alpn_quic_list[] = {
    2, 'h', '3'
};
#endif

/**
 * Write a formatted error line into the caller's buffer, tacking on the
 * most recent OpenSSL error if the stack has one. Both halves are
 * bounded by @p err_cap, so the output is always NUL-terminated.
 */
static void tls_format_error(char *err_buf, size_t err_cap,
                             const char *prefix, const char *detail)
{
    if (err_buf == NULL || err_cap == 0) {
        return;
    }

    const unsigned long openssl_err = ERR_peek_last_error();
    char openssl_msg[160] = {0};
    if (openssl_err != 0) {
        ERR_error_string_n(openssl_err, openssl_msg, sizeof(openssl_msg));
    }

    if (detail != NULL && openssl_msg[0] != '\0') {
        snprintf(err_buf, err_cap, "%s: %s (%s)", prefix, detail, openssl_msg);
    } else if (detail != NULL) {
        snprintf(err_buf, err_cap, "%s: %s", prefix, detail);
    } else if (openssl_msg[0] != '\0') {
        snprintf(err_buf, err_cap, "%s: %s", prefix, openssl_msg);
    } else {
        snprintf(err_buf, err_cap, "%s", prefix);
    }

    /* Drain any residue so later calls don't pick up stale errors. */
    ERR_clear_error();
}

#ifdef HAVE_HTTP_SERVER_HTTP3
/* Per-SSL marker set by http3_connection_attach_tls right after SSL_new,
 * so the ALPN callback can pick the QUIC list. We can't rely on
 * SSL_is_quic() — that returns true only for SSL objects driving the
 * native OpenSSL QUIC stack (QUIC_server_method etc). When ngtcp2 owns
 * the QUIC transport and uses the OpenSSL 3.5 "QUIC TLS callbacks" API
 * (SSL_set_quic_tls_cbs, via ngtcp2_crypto_ossl_configure_server_session)
 * the SSL is still a regular TLS handle and SSL_is_quic returns 0. */
static int tls_quic_ex_idx = -1;

int tls_layer_quic_ex_index(void)
{
    if (tls_quic_ex_idx == -1) {
        tls_quic_ex_idx = SSL_get_ex_new_index(0, (void *)"http3-quic", NULL, NULL, NULL);
    }
    return tls_quic_ex_idx;
}

void tls_layer_mark_ssl_quic(SSL *ssl)
{
    SSL_set_ex_data(ssl, tls_layer_quic_ex_index(), (void *)1);
}
#endif

/**
 * ALPN server-side selector. Picks a transport-appropriate list
 * (TCP → #tls_alpn_tcp_list; QUIC → #tls_alpn_quic_list) then chooses
 * the first entry from it that the client also offers. Returns
 * SSL_TLSEXT_ERR_ALERT_FATAL (not NOACK) so a client that sends ALPN
 * but speaks none of our protocols gets a clean handshake failure
 * instead of an unprotocoled connection — see RFC 7301 §3.2.
 */
static int tls_alpn_select_cb(SSL *ssl,
                              const unsigned char **out, unsigned char *outlen,
                              const unsigned char *in, unsigned int inlen,
                              void *arg)
{
    (void)arg;

    const uint8_t *advertised;
    unsigned int   advertised_len;

#ifdef HAVE_HTTP_SERVER_HTTP3
    /* The QUIC bit is set per-SSL by tls_layer_mark_ssl_quic, called from
     * http3_connection_attach_tls before the handshake starts. */
    if (SSL_get_ex_data(ssl, tls_layer_quic_ex_index()) != NULL) {
        advertised     = tls_alpn_quic_list;
        advertised_len = (unsigned int)sizeof(tls_alpn_quic_list);
    } else
#endif
    {
        advertised     = tls_alpn_tcp_list;
        advertised_len = (unsigned int)sizeof(tls_alpn_tcp_list);
    }

    unsigned char *selected = NULL;
    unsigned char selected_len = 0;
    const int rc = SSL_select_next_proto(&selected, &selected_len,
                                         advertised, advertised_len,
                                         in, inlen);
    if (rc != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    *out = selected;
    *outlen = selected_len;
    return SSL_TLSEXT_ERR_OK;
}

/**
 * Apply the hardened default options to @p ssl_ctx. Split out so the
 * policy is readable at a glance.
 */
static bool tls_apply_security_defaults(SSL_CTX *const ssl_ctx,
                                        char *err_buf, size_t err_cap)
{
    if (SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION) != 1) {
        tls_format_error(err_buf, err_cap,
                         "Failed to pin TLS protocol range", NULL);
        return false;
    }

    /* No SSL_OP_NO_TICKET_FOR_TLS12 in stock OpenSSL — would need a
     * ticket_cb. TLS 1.3 tickets are stateless by default (see the
     * session-cache mode below), which covers the deployment pattern
     * we care about; TLS 1.2 ticket hygiene is a follow-up. */
    uint64_t options =
        SSL_OP_NO_COMPRESSION          /* CRIME */
      | SSL_OP_NO_RENEGOTIATION        /* CVE-2009-3555 family */
      | SSL_OP_CIPHER_SERVER_PREFERENCE;

#ifdef HAVE_KTLS
    /* OpenSSL feature-detects the kernel at handshake time; if the
     * kernel refuses, the flag has no effect — safe to always set. */
    options |= SSL_OP_ENABLE_KTLS;
#endif

    SSL_CTX_set_options(ssl_ctx, options);

    /* Reject SHA1 in chains, RSA <2048, and other <112-bit primitives.
     * OpenSSL default is level 1 (legacy). */
    SSL_CTX_set_security_level(ssl_ctx, 2);

    if (SSL_CTX_set_ciphersuites(ssl_ctx, tls_ciphersuites_v13) != 1) {
        tls_format_error(err_buf, err_cap,
                         "Failed to set TLS 1.3 ciphersuites", NULL);
        return false;
    }
    if (SSL_CTX_set_cipher_list(ssl_ctx, tls_cipher_list_v12) != 1) {
        tls_format_error(err_buf, err_cap,
                         "Failed to set TLS 1.2 cipher list", NULL);
        return false;
    }

    /* Stateless session tickets only — no server-side session store.
     * Matches horizontally scaled / load-balanced deployments where
     * ticket keys are the only shared state. */
    SSL_CTX_set_session_cache_mode(ssl_ctx,
        SSL_SESS_CACHE_SERVER | SSL_SESS_CACHE_NO_INTERNAL_STORE);

    /* TLS 1.3: advertise two NewSessionTicket messages per handshake
     * (OpenSSL default). One isn't enough for clients that open two
     * parallel resumed connections; more than two wastes bytes. */
    SSL_CTX_set_num_tickets(ssl_ctx, 2);

    /* 0-RTT off by default — re-enable later with explicit anti-replay
     * handling. */

    SSL_CTX_set_alpn_select_cb(ssl_ctx, tls_alpn_select_cb, NULL);

    return true;
}

/* TLS 1.2 / TLS 1.3 ticket key rotation (3.5).
 *
 * OpenSSL's default ticket-key handling generates a single key per
 * SSL_CTX at init and never rotates it: a process running for months
 * encrypts every issued ticket with the same key. If that key ever
 * leaks (memory disclosure, side channel, post-mortem crash dump on a
 * shared host), every captured ticket-resumed session can be decrypted
 * retroactively — a forward-secrecy hole.
 *
 * We replace the default with a key-set + lazy-rotation scheme:
 *  - 3 slots, slot 0 is current.
 *  - At ticket-issue time we check elapsed wall-clock; if rotation
 *    period elapsed, shift (slot N-1 cleansed, slots 1..N-1 take
 *    previous slots, slot 0 generated fresh). Old tickets remain
 *    decryptable for one rotation period.
 *  - Encrypted with slot 0; decrypted by scanning slot 0..N-1 for the
 *    matching key_name. Stale clients (slot >0) get their ticket
 *    re-issued via return value 2.
 *
 * No per-listener timer — rotation is driven by ticket traffic itself.
 * The window is 4h; on a quiet listener the same slot 0 may live
 * longer, which is fine because no new tickets are being issued. */
#define TLS_TICKET_ROTATE_PERIOD_NS (4ull * 3600ull * 1000000000ull)

static int tls_ticket_ex_idx = -1;

static bool tls_ticket_key_generate(tls_ticket_key_t *k)
{
    if (RAND_bytes(k->name, sizeof(k->name)) != 1) return false;
    if (RAND_bytes(k->aes_key, sizeof(k->aes_key)) != 1) return false;
    if (RAND_bytes(k->hmac_key, sizeof(k->hmac_key)) != 1) return false;
    k->valid = true;
    return true;
}

static void tls_ticket_keys_rotate(tls_context_t *ctx)
{
    /* Cleanse oldest, shift, generate new at slot 0. On RAND failure
     * leave slot 0 valid as before (degraded, not broken). */
    OPENSSL_cleanse(&ctx->ticket_keys[TLS_TICKET_KEY_SLOTS - 1],
                    sizeof(ctx->ticket_keys[0]));
    for (int i = TLS_TICKET_KEY_SLOTS - 1; i > 0; i--) {
        ctx->ticket_keys[i] = ctx->ticket_keys[i - 1];
    }
    memset(&ctx->ticket_keys[0], 0, sizeof(ctx->ticket_keys[0]));
    if (!tls_ticket_key_generate(&ctx->ticket_keys[0])) {
        /* Recover slot 0 from slot 1 so encrypt does not fail. */
        if (ctx->ticket_keys[1].valid) {
            ctx->ticket_keys[0] = ctx->ticket_keys[1];
        }
    }
    ctx->ticket_keys_last_rotation_ns = (uint64_t)zend_hrtime();
}

static void tls_maybe_rotate_ticket_keys(tls_context_t *ctx)
{
    const uint64_t now = (uint64_t)zend_hrtime();
    if (ctx->ticket_keys_last_rotation_ns != 0
        && (now - ctx->ticket_keys_last_rotation_ns) < TLS_TICKET_ROTATE_PERIOD_NS) {
        return;
    }
    tls_ticket_keys_rotate(ctx);
}

static int tls_ticket_key_cb(SSL *s, unsigned char *key_name,
                             unsigned char *iv,
                             EVP_CIPHER_CTX *cipher_ctx,
                             EVP_MAC_CTX *mac_ctx, int enc)
{
    SSL_CTX *ssl_ctx = SSL_get_SSL_CTX(s);
    tls_context_t *ctx =
        (tls_context_t *)SSL_CTX_get_ex_data(ssl_ctx, tls_ticket_ex_idx);
    if (UNEXPECTED(ctx == NULL)) {
        return -1;
    }

    OSSL_PARAM mac_params[3];
    mac_params[1] = OSSL_PARAM_construct_end();

    if (enc == 1) {
        tls_maybe_rotate_ticket_keys(ctx);
        tls_ticket_key_t *const k = &ctx->ticket_keys[0];
        if (!k->valid) return -1;

        memcpy(key_name, k->name, sizeof(k->name));
        if (RAND_bytes(iv, EVP_CIPHER_iv_length(EVP_aes_256_cbc())) != 1) {
            return -1;
        }
        if (EVP_EncryptInit_ex(cipher_ctx, EVP_aes_256_cbc(), NULL,
                               k->aes_key, iv) != 1) {
            return -1;
        }
        mac_params[0] = OSSL_PARAM_construct_octet_string(
            OSSL_MAC_PARAM_KEY, k->hmac_key, sizeof(k->hmac_key));
        mac_params[1] = OSSL_PARAM_construct_utf8_string(
            OSSL_MAC_PARAM_DIGEST, "SHA256", 0);
        mac_params[2] = OSSL_PARAM_construct_end();
        if (EVP_MAC_CTX_set_params(mac_ctx, mac_params) != 1) {
            return -1;
        }
        return 1;
    }

    /* Decrypt: constant-time scan for matching key_name. */
    int hit = -1;
    for (int i = 0; i < TLS_TICKET_KEY_SLOTS; i++) {
        if (!ctx->ticket_keys[i].valid) continue;
        if (CRYPTO_memcmp(key_name, ctx->ticket_keys[i].name,
                          sizeof(ctx->ticket_keys[i].name)) == 0) {
            hit = i;
            break;
        }
    }
    if (hit < 0) {
        return 0; /* unknown key — full handshake */
    }
    tls_ticket_key_t *const k = &ctx->ticket_keys[hit];
    if (EVP_DecryptInit_ex(cipher_ctx, EVP_aes_256_cbc(), NULL,
                           k->aes_key, iv) != 1) {
        return -1;
    }
    mac_params[0] = OSSL_PARAM_construct_octet_string(
        OSSL_MAC_PARAM_KEY, k->hmac_key, sizeof(k->hmac_key));
    mac_params[1] = OSSL_PARAM_construct_utf8_string(
        OSSL_MAC_PARAM_DIGEST, "SHA256", 0);
    mac_params[2] = OSSL_PARAM_construct_end();
    if (EVP_MAC_CTX_set_params(mac_ctx, mac_params) != 1) {
        return -1;
    }
    /* hit > 0 → ticket was issued under an older key; tell OpenSSL to
     * re-issue under the current key on this resumption. */
    return hit > 0 ? 2 : 1;
}

static bool tls_install_ticket_key_cb(tls_context_t *ctx, char *err_buf,
                                      size_t err_cap)
{
    if (tls_ticket_ex_idx < 0) {
        tls_ticket_ex_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
        if (tls_ticket_ex_idx < 0) {
            tls_format_error(err_buf, err_cap,
                "SSL_CTX_get_ex_new_index failed for ticket-key slot", NULL);
            return false;
        }
    }
    if (SSL_CTX_set_ex_data(ctx->ctx, tls_ticket_ex_idx, ctx) != 1) {
        tls_format_error(err_buf, err_cap,
            "SSL_CTX_set_ex_data failed (ticket-key slot)", NULL);
        return false;
    }
    if (!tls_ticket_key_generate(&ctx->ticket_keys[0])) {
        tls_format_error(err_buf, err_cap,
            "OpenSSL RAND_bytes failed seeding ticket key", NULL);
        return false;
    }
    ctx->ticket_keys_last_rotation_ns = (uint64_t)zend_hrtime();

    if (SSL_CTX_set_tlsext_ticket_key_evp_cb(ctx->ctx, tls_ticket_key_cb) != 1) {
        tls_format_error(err_buf, err_cap,
            "SSL_CTX_set_tlsext_ticket_key_evp_cb failed", NULL);
        return false;
    }
    return true;
}

tls_context_t *tls_context_new(const char *cert_path,
                               const char *key_path,
                               char *err_buf,
                               size_t err_cap)
{
    if (cert_path == NULL || *cert_path == '\0') {
        tls_format_error(err_buf, err_cap,
                         "TLS certificate path is not configured", NULL);
        return NULL;
    }
    if (key_path == NULL || *key_path == '\0') {
        tls_format_error(err_buf, err_cap,
                         "TLS private key path is not configured", NULL);
        return NULL;
    }

    ERR_clear_error();

#ifdef HAVE_HTTP_SERVER_HTTP3
    /* ngtcp2_crypto_ossl_init must run BEFORE the first SSL_CTX_new of
     * any context that will host a QUIC SSL — it registers the
     * OpenSSL-3.5 QUIC TLS callbacks that ngtcp2_crypto_ossl_configure_
     * server_session relies on. Calling it later (lazily on first
     * datagram) leaves the SSL_CTX without the QUIC hook, so
     * SSL_provide_quic_data never advances the TLS state machine and
     * ServerHello is never emitted. Idempotent. */
    extern int ngtcp2_crypto_ossl_init(void);
    (void)ngtcp2_crypto_ossl_init();
#endif

    SSL_CTX *const ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (ssl_ctx == NULL) {
        tls_format_error(err_buf, err_cap, "SSL_CTX_new failed", NULL);
        return NULL;
    }

    if (!tls_apply_security_defaults(ssl_ctx, err_buf, err_cap)) {
        SSL_CTX_free(ssl_ctx);
        return NULL;
    }

    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_path) != 1) {
        tls_format_error(err_buf, err_cap,
                         "Failed to load certificate chain", cert_path);
        SSL_CTX_free(ssl_ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        tls_format_error(err_buf, err_cap,
                         "Failed to load private key", key_path);
        SSL_CTX_free(ssl_ctx);
        return NULL;
    }

    if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
        tls_format_error(err_buf, err_cap,
                         "Private key does not match certificate", NULL);
        SSL_CTX_free(ssl_ctx);
        return NULL;
    }

    tls_context_t *const ctx = pemalloc(sizeof(*ctx), 0);
    memset(ctx, 0, sizeof(*ctx));
    ctx->ctx       = ssl_ctx;
    ctx->cert_path = zend_string_init(cert_path, strlen(cert_path), 0);
    ctx->key_path  = zend_string_init(key_path, strlen(key_path), 0);
#ifdef HAVE_KTLS
    ctx->ktls_enabled = true;
#endif

    if (!tls_install_ticket_key_cb(ctx, err_buf, err_cap)) {
        tls_context_free(ctx);
        return NULL;
    }

    return ctx;
}

void tls_context_free(tls_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->ctx != NULL) {
        SSL_CTX_free(ctx->ctx);
        ctx->ctx = NULL;
    }
    if (ctx->cert_path != NULL) {
        zend_string_release(ctx->cert_path);
        ctx->cert_path = NULL;
    }
    if (ctx->key_path != NULL) {
        zend_string_release(ctx->key_path);
        ctx->key_path = NULL;
    }

    OPENSSL_cleanse(ctx->ticket_keys, sizeof(ctx->ticket_keys));

    pefree(ctx, 0);
}

/* -------------------------------------------------------------------------
 * Per-connection session (BIO pair + I/O state machine).
 * ------------------------------------------------------------------------- */

static const char *const tls_op_names_table[] = {
    [TLS_OP_NONE]      = "none",
    [TLS_OP_HANDSHAKE] = "handshake",
    [TLS_OP_READ]      = "read",
    [TLS_OP_WRITE]     = "write",
    [TLS_OP_COMMIT]    = "commit",
    [TLS_OP_CONSUME]   = "consume",
    [TLS_OP_SHUTDOWN]  = "shutdown",
};

const char *tls_op_name(const tls_op_t op)
{
    if ((size_t)op >= (sizeof(tls_op_names_table) / sizeof(tls_op_names_table[0]))) {
        return "unknown";
    }
    const char *const name = tls_op_names_table[op];
    return name != NULL ? name : "unknown";
}

/**
 * Record a structured error on the session and drain the OpenSSL error
 * stack. Must be called before any subsequent SSL_* call that would
 * push new errors.
 *
 * @p ssl_err is the SSL_get_error() code for SSL-side failures, or -1
 * for BIO-side failures (commit/consume).
 * @p bytes_done is how many bytes the caller had already successfully
 * processed inside the failing operation before the error — useful for
 * diagnosing truncation vs mid-record failures.
 * @p fallback_reason is used when ERR_peek_last_error() returns 0 (BIO
 * primitives often fail without pushing an error). May be NULL.
 */
static void tls_session_record_error(tls_session_t *const session,
                                     const tls_op_t op,
                                     const int ssl_err,
                                     const size_t bytes_done,
                                     const char *const fallback_reason)
{
    if (session == NULL) {
        return;
    }
    tls_error_info_t *const err = &session->last_error;
    err->op            = op;
    err->ssl_err       = ssl_err;
    err->openssl_err   = ERR_peek_last_error();
    err->state_at_fail = session->state;
    err->bytes_done    = bytes_done;

    if (err->openssl_err != 0) {
        ERR_error_string_n(err->openssl_err, err->reason, sizeof(err->reason));
    } else if (fallback_reason != NULL) {
        (void)snprintf(err->reason, sizeof(err->reason), "%s", fallback_reason);
    } else {
        err->reason[0] = '\0';
    }

    ERR_clear_error();
}

/**
 * Map the last SSL_get_error() verdict for @p session onto our public
 * enum and, on hard error, capture it into session->last_error.
 *
 * Centralised so every primitive has identical observable behaviour on
 * WANT_READ / WANT_WRITE / ZERO_RETURN; anything else is an error. The
 * OpenSSL error stack is drained (inside tls_session_record_error) to
 * prevent stale entries from leaking into the next operation.
 */
static tls_io_result_t tls_classify_ssl_error(tls_session_t *const session,
                                              const int rc,
                                              const tls_op_t op,
                                              const size_t bytes_done)
{
    const int code = SSL_get_error(session->ssl, rc);
    switch (code) {
        case SSL_ERROR_NONE:
            return TLS_IO_OK;
        case SSL_ERROR_WANT_READ:
            return TLS_IO_WANT_READ;
        case SSL_ERROR_WANT_WRITE:
            return TLS_IO_WANT_WRITE;
        case SSL_ERROR_ZERO_RETURN:
            return TLS_IO_CLOSED;
        case SSL_ERROR_SSL:
        case SSL_ERROR_SYSCALL:
        default:
            tls_session_record_error(session, op, code, bytes_done,
                code == SSL_ERROR_SYSCALL ? "SSL_ERROR_SYSCALL (no OpenSSL detail)"
                                          : NULL);
            return TLS_IO_ERROR;
    }
}

/**
 * Recognise the ALPN the handshake settled on. Called once the server
 * transitions to TLS_ESTABLISHED; after that the protocol strategy
 * dispatches based on session->alpn_selected.
 */
static tls_alpn_t tls_lookup_alpn(SSL *const ssl)
{
    const unsigned char *proto = NULL;
    unsigned int proto_len = 0;
    SSL_get0_alpn_selected(ssl, &proto, &proto_len);
    if (proto == NULL || proto_len == 0) {
        return TLS_ALPN_NONE;
    }
    if (proto_len == 8 && memcmp(proto, "http/1.1", 8) == 0) {
        return TLS_ALPN_HTTP11;
    }
    if (proto_len == 2 && memcmp(proto, "h2", 2) == 0) {
        return TLS_ALPN_H2;
    }
    if (proto_len == 2 && memcmp(proto, "h3", 2) == 0) {
        return TLS_ALPN_H3;
    }
    return TLS_ALPN_NONE;
}

tls_session_t *tls_session_new(tls_context_t *ctx)
{
    if (ctx == NULL || ctx->ctx == NULL) {
        return NULL;
    }

    SSL *const ssl = SSL_new(ctx->ctx);
    if (ssl == NULL) {
        ERR_clear_error();
        return NULL;
    }

    BIO *internal_bio = NULL;
    BIO *network_bio  = NULL;
    if (BIO_new_bio_pair(&internal_bio, TLS_BIO_RING_SIZE,
                         &network_bio,  TLS_BIO_RING_SIZE) != 1) {
        SSL_free(ssl);
        ERR_clear_error();
        return NULL;
    }

    /* SSL_set_bio() takes ownership of internal_bio — SSL_free()
     * will BIO_free() it. The BIO pair we created has the property
     * that freeing one half does NOT free the other, so network_bio
     * remains our responsibility and is freed in tls_session_free(). */
    SSL_set_bio(ssl, internal_bio, internal_bio);
    SSL_set_accept_state(ssl);

    tls_session_t *const session = pemalloc(sizeof(*session), 0);
    *session = (tls_session_t){
        .ssl           = ssl,
        .internal_bio  = internal_bio,
        .network_bio   = network_bio,
        .state         = TLS_HANDSHAKING,
        .alpn_selected = TLS_ALPN_NONE,
    };
    return session;
}

void tls_session_free(tls_session_t *session)
{
    if (session == NULL) {
        return;
    }

    if (session->ssl != NULL) {
        /* Frees internal_bio along with ssl. */
        SSL_free(session->ssl);
        session->ssl = NULL;
        session->internal_bio = NULL;
    }
    if (session->network_bio != NULL) {
        BIO_free(session->network_bio);
        session->network_bio = NULL;
    }

    pefree(session, 0);
}

tls_io_result_t tls_feed_ciphertext(tls_session_t *session,
                                    const char *buf, size_t len,
                                    size_t *consumed)
{
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (session == NULL || session->network_bio == NULL) {
        return TLS_IO_ERROR;
    }
    if (len == 0) {
        return TLS_IO_OK;
    }
    if (len > INT_MAX) {
        len = INT_MAX;
    }

    const int written = BIO_write(session->network_bio, buf, (int)len);
    if (written <= 0) {
        /* BIO pair is full. Caller must drain plaintext (consuming
         * ciphertext from OpenSSL's side) before pushing more. */
        if (BIO_should_retry(session->network_bio)) {
            return TLS_IO_WANT_READ;
        }
        return TLS_IO_ERROR;
    }

    if (consumed != NULL) {
        *consumed = (size_t)written;
    }
    return TLS_IO_OK;
}

tls_io_result_t tls_drain_ciphertext(tls_session_t *session,
                                     char *buf, size_t cap,
                                     size_t *produced)
{
    if (produced != NULL) {
        *produced = 0;
    }
    if (session == NULL || session->network_bio == NULL) {
        return TLS_IO_ERROR;
    }
    if (cap == 0) {
        return TLS_IO_OK;
    }
    if (cap > INT_MAX) {
        cap = INT_MAX;
    }

    const int read_n = BIO_read(session->network_bio, buf, (int)cap);
    if (read_n <= 0) {
        /* Nothing pending — not an error, OpenSSL simply has no
         * encrypted output right now. */
        if (BIO_should_retry(session->network_bio)) {
            return TLS_IO_OK;
        }
        return TLS_IO_ERROR;
    }

    if (produced != NULL) {
        *produced = (size_t)read_n;
    }
    return TLS_IO_OK;
}

tls_io_result_t tls_handshake_step(tls_session_t *session)
{
    if (session == NULL || session->ssl == NULL) {
        return TLS_IO_ERROR;
    }
    if (session->state == TLS_ESTABLISHED) {
        return TLS_IO_OK;
    }
    if (session->state == TLS_CLOSED) {
        return TLS_IO_CLOSED;
    }

    ERR_clear_error();
    const int rc = SSL_do_handshake(session->ssl);
    if (rc == 1) {
        session->state = TLS_ESTABLISHED;
        session->alpn_selected = tls_lookup_alpn(session->ssl);
        return TLS_IO_OK;
    }

    return tls_classify_ssl_error(session, rc, TLS_OP_HANDSHAKE, 0);
}

tls_io_result_t tls_read_plaintext(tls_session_t *session,
                                   char *buf, size_t cap,
                                   size_t *produced)
{
    if (produced != NULL) {
        *produced = 0;
    }
    if (session == NULL || session->ssl == NULL) {
        return TLS_IO_ERROR;
    }
    if (session->state == TLS_CLOSED) {
        return TLS_IO_CLOSED;
    }
    if (cap == 0) {
        return TLS_IO_OK;
    }
    if (cap > INT_MAX) {
        cap = INT_MAX;
    }

    ERR_clear_error();
    size_t read_n = 0;
    const int rc = SSL_read_ex(session->ssl, buf, cap, &read_n);
    if (rc == 1) {
        if (produced != NULL) {
            *produced = read_n;
        }
        return TLS_IO_OK;
    }

    const tls_io_result_t mapped = tls_classify_ssl_error(session, rc, TLS_OP_READ, read_n);
    if (mapped == TLS_IO_CLOSED) {
        session->state = TLS_CLOSED;
    }
    return mapped;
}

tls_io_result_t tls_write_plaintext(tls_session_t *session,
                                    const char *buf, size_t len,
                                    size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (session == NULL || session->ssl == NULL) {
        return TLS_IO_ERROR;
    }
    if (session->state == TLS_CLOSED || session->state == TLS_SHUTTING_DOWN) {
        return TLS_IO_CLOSED;
    }
    if (len == 0) {
        return TLS_IO_OK;
    }
    if (len > INT_MAX) {
        len = INT_MAX;
    }

    ERR_clear_error();
    size_t wrote = 0;
    const int rc = SSL_write_ex(session->ssl, buf, len, &wrote);
    if (rc == 1) {
        if (written != NULL) {
            *written = wrote;
        }
        return TLS_IO_OK;
    }

    return tls_classify_ssl_error(session, rc, TLS_OP_WRITE, wrote);
}

/* -------------------------------------------------------------------------
 * Zero-copy BIO helpers.
 *
 * Callers get a direct pointer into the BIO pair's internal ring and do
 * the socket I/O there — no intermediate buffer. The ring never wraps
 * a peeked region across its boundary, so callers may have to loop on
 * backpressure, but for a single event-loop tick one call is enough.
 *
 * Returning int from BIO_n*  — we clamp negative/overflow to 0 at the
 * callsite; OpenSSL 3.x returns < 0 only on unrecoverable errors which
 * our single-owner session-model never hits (the peer side of the pair
 * outlives us and stays attached to SSL*).
 * ------------------------------------------------------------------------- */

size_t tls_reserve_cipher_in(tls_session_t *const session, char **const out_ptr)
{
    if (session == NULL || session->network_bio == NULL || out_ptr == NULL) {
        return 0;
    }
    char *slot = NULL;
    const int space = BIO_nwrite0(session->network_bio, &slot);
    if (space <= 0 || slot == NULL) {
        *out_ptr = NULL;
        return 0;
    }
    *out_ptr = slot;
    return (size_t)space;
}

bool tls_commit_cipher_in(tls_session_t *const session, const size_t n)
{
    if (session == NULL || session->network_bio == NULL) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    char *dummy = NULL;
    int to_commit = (n > (size_t)INT_MAX) ? INT_MAX : (int)n;
    /* Caller reserved at least `n` bytes via tls_reserve_cipher_in and
     * did not issue any other write-side operation before this commit
     * (single-owner invariant). BIO_nwrite normally commits everything
     * in one shot; the loop absorbs the ring-wrap case. If BIO_nwrite
     * returns <= 0 the ring state is inconsistent
     * (out-of-memory inside OpenSSL, BIO torn down under us): mark the
     * session CLOSED so the state machine propagates the error through
     * the next handshake/read/write step, drain the OpenSSL error
     * stack, and return failure so the caller can stop feeding. */
    const int total = to_commit;
    while (to_commit > 0) {
        const int committed = BIO_nwrite(session->network_bio, &dummy, to_commit);
        if (committed <= 0) {
            tls_session_record_error(session, TLS_OP_COMMIT, -1,
                                     (size_t)(total - to_commit),
                                     "BIO_nwrite rejected commit after reserve");
            session->state = TLS_CLOSED;
            return false;
        }
        to_commit -= committed;
    }
    return true;
}

size_t tls_peek_cipher_out(tls_session_t *const session, char **const out_ptr)
{
    if (session == NULL || session->network_bio == NULL || out_ptr == NULL) {
        return 0;
    }
    char *slot = NULL;
    const int avail = BIO_nread0(session->network_bio, &slot);
    if (avail <= 0 || slot == NULL) {
        *out_ptr = NULL;
        return 0;
    }
    *out_ptr = slot;
    return (size_t)avail;
}

bool tls_session_was_resumed(const tls_session_t *const session)
{
    if (session == NULL || session->ssl == NULL) {
        return false;
    }
    return SSL_session_reused(session->ssl) == 1;
}

/* BIO_get_ktls_{send,recv} are macros that resolve to 0 when OpenSSL
 * was built with OPENSSL_NO_KTLS. When OpenSSL is kTLS-capable but the
 * kernel refuses at handshake time (old kernel, tls module missing,
 * BIO pair layout that precludes direct offload), they also return 0.
 * Both cases manifest identically to the caller: "no offload, keep
 * using userspace SSL_write/SSL_read" — exactly what we want. */
bool tls_session_ktls_tx_active(const tls_session_t *const session)
{
    if (session == NULL || session->internal_bio == NULL) {
        return false;
    }
    return BIO_get_ktls_send(session->internal_bio) != 0;
}

bool tls_session_ktls_rx_active(const tls_session_t *const session)
{
    if (session == NULL || session->internal_bio == NULL) {
        return false;
    }
    return BIO_get_ktls_recv(session->internal_bio) != 0;
}

bool tls_consume_cipher_out(tls_session_t *const session, const size_t n)
{
    if (session == NULL || session->network_bio == NULL) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    char *dummy = NULL;
    int to_consume = (n > (size_t)INT_MAX) ? INT_MAX : (int)n;
    /* Mirror of tls_commit_cipher_in: caller peeked `n` bytes and sent
     * exactly that many. Same failure policy — mark session CLOSED,
     * drain OpenSSL error stack, return false. */
    const int total = to_consume;
    while (to_consume > 0) {
        const int consumed = BIO_nread(session->network_bio, &dummy, to_consume);
        if (consumed <= 0) {
            tls_session_record_error(session, TLS_OP_CONSUME, -1,
                                     (size_t)(total - to_consume),
                                     "BIO_nread rejected consume after peek");
            session->state = TLS_CLOSED;
            return false;
        }
        to_consume -= consumed;
    }
    return true;
}

tls_io_result_t tls_shutdown_step(tls_session_t *session)
{
    if (session == NULL || session->ssl == NULL) {
        return TLS_IO_ERROR;
    }
    if (session->state == TLS_CLOSED) {
        return TLS_IO_CLOSED;
    }
    session->state = TLS_SHUTTING_DOWN;

    ERR_clear_error();
    const int rc = SSL_shutdown(session->ssl);
    if (rc == 1) {
        /* Both close_notifys exchanged. */
        session->state = TLS_CLOSED;
        return TLS_IO_CLOSED;
    }
    if (rc == 0) {
        /* Our close_notify was sent; peer's has not arrived yet. Caller
         * should drain ciphertext, arm a read, and call us again. */
        return TLS_IO_WANT_READ;
    }

    return tls_classify_ssl_error(session, rc, TLS_OP_SHUTDOWN, 0);
}

const tls_error_info_t *tls_session_last_error(const tls_session_t *const session)
{
    if (session == NULL) {
        return NULL;
    }
    return &session->last_error;
}

#endif /* HAVE_OPENSSL */
