/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/**
 * @file test_tls_session.c
 * @brief Offline drive of the tls_session_t state machine (PLAN_TLS §Step 3).
 *
 * Every test wires one server-side tls_session_t (our production API)
 * against a raw client-side SSL* + BIO pair (test-only helper) and
 * shuttles bytes between them — no sockets, no event loop. This is the
 * cheapest environment that exercises real OpenSSL state transitions
 * and keeps ASan/UBSan verdicts about *our* code isolated from any I/O
 * layer.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "common/php_sapi_test.h"
#include "core/tls_layer.h"

/* -------------------------------------------------------------------------
 * Test fixture: self-signed cert minted once per suite run and dropped
 * into mkstemp() temp files so tls_context_new() can load it by path.
 * ------------------------------------------------------------------------- */

typedef struct {
    char cert_path[64];
    char key_path[64];
} tls_test_cert_t;

static tls_test_cert_t suite_cert;

/**
 * Mint an RSA-2048 self-signed certificate valid for one hour and write
 * the PEM cert + key pair into the paths inside @p out.
 */
static int mint_self_signed_cert(tls_test_cert_t *const out)
{
    strcpy(out->cert_path, "/tmp/tls_test_cert_XXXXXX");
    strcpy(out->key_path,  "/tmp/tls_test_key_XXXXXX");
    const int cert_fd = mkstemp(out->cert_path);
    const int key_fd  = mkstemp(out->key_path);
    if (cert_fd < 0 || key_fd < 0) {
        return -1;
    }
    close(cert_fd);
    close(key_fd);

    EVP_PKEY *const pkey = EVP_RSA_gen(2048);
    if (pkey == NULL) {
        return -1;
    }

    X509 *const x509 = X509_new();
    if (x509 == NULL) {
        EVP_PKEY_free(pkey);
        return -1;
    }

    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
    X509_set_pubkey(x509, pkey);

    X509_NAME *const name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *)"localhost", -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    FILE *const cert_fp = fopen(out->cert_path, "w");
    FILE *const key_fp  = fopen(out->key_path,  "w");
    if (cert_fp == NULL || key_fp == NULL) {
        if (cert_fp) fclose(cert_fp);
        if (key_fp)  fclose(key_fp);
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return -1;
    }

    PEM_write_X509(cert_fp, x509);
    PEM_write_PrivateKey(key_fp, pkey, NULL, NULL, 0, NULL, NULL);
    fclose(cert_fp);
    fclose(key_fp);

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return 0;
}

/* -------------------------------------------------------------------------
 * Client harness — a raw SSL*/ /* with its own BIO pair. We DO NOT want
 * to reuse the server tls_session_t code here: the test needs a
 * second, independently controlled TLS endpoint to be a meaningful
 * integration of our server-side state machine.
 * ------------------------------------------------------------------------- */

typedef struct {
    SSL_CTX *ctx;
    SSL     *ssl;
    BIO     *internal;
    BIO     *network;
} tls_test_client_t;

static void client_init(tls_test_client_t *const c)
{
    c->ctx = SSL_CTX_new(TLS_client_method());
    assert_non_null(c->ctx);
    /* Server cert is self-signed — tell the client to accept it. The
     * unit under test is the server handshake, not cert validation. */
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(c->ctx, TLS1_3_VERSION);
    static const uint8_t alpn_http11[] = { 8, 'h','t','t','p','/','1','.','1' };
    SSL_CTX_set_alpn_protos(c->ctx, alpn_http11, sizeof(alpn_http11));

    c->ssl = SSL_new(c->ctx);
    assert_non_null(c->ssl);

    const int rc = BIO_new_bio_pair(&c->internal, TLS_BIO_RING_SIZE,
                                    &c->network,  TLS_BIO_RING_SIZE);
    assert_int_equal(rc, 1);
    SSL_set_bio(c->ssl, c->internal, c->internal);
    SSL_set_connect_state(c->ssl);
}

static void client_free(tls_test_client_t *const c)
{
    if (c->ssl)     SSL_free(c->ssl);      /* frees c->internal */
    if (c->network) BIO_free(c->network);
    if (c->ctx)     SSL_CTX_free(c->ctx);
    *c = (tls_test_client_t){0};
}

/* Move whatever ciphertext sits on one endpoint's network-side BIO into
 * the other's. Returns total bytes moved (for progress detection). */
static size_t shuttle(tls_session_t *const server, tls_test_client_t *const client)
{
    size_t moved = 0;
    char buf[TLS_BIO_RING_SIZE];

    /* server → client */
    while (1) {
        size_t produced = 0;
        const tls_io_result_t rc = tls_drain_ciphertext(
            server, buf, sizeof(buf), &produced);
        if (rc != TLS_IO_OK || produced == 0) break;
        const int written = BIO_write(client->network, buf, (int)produced);
        assert_int_equal(written, (int)produced);
        moved += produced;
    }

    /* client → server */
    while (1) {
        const int got = BIO_read(client->network, buf, sizeof(buf));
        if (got <= 0) break;
        size_t consumed = 0;
        const tls_io_result_t rc = tls_feed_ciphertext(
            server, buf, (size_t)got, &consumed);
        assert_true(rc == TLS_IO_OK || rc == TLS_IO_WANT_READ);
        assert_int_equal((int)consumed, got);
        moved += consumed;
    }
    return moved;
}

/**
 * Pump until both sides are either established or progress stalls. The
 * test-case assertion decides whether "stalls" is a pass or a fail.
 */
static int pump_handshake(tls_session_t *const server,
                          tls_test_client_t *const client,
                          int max_rounds)
{
    int rounds = 0;
    while (rounds < max_rounds) {
        rounds++;

        /* Advance each side. */
        const tls_io_result_t server_rc = tls_handshake_step(server);
        assert_true(server_rc == TLS_IO_OK ||
                    server_rc == TLS_IO_WANT_READ ||
                    server_rc == TLS_IO_WANT_WRITE);

        ERR_clear_error();
        const int client_rc = SSL_do_handshake(client->ssl);
        if (client_rc != 1) {
            const int code = SSL_get_error(client->ssl, client_rc);
            assert_true(code == SSL_ERROR_WANT_READ ||
                        code == SSL_ERROR_WANT_WRITE);
        }

        /* Shuttle bytes. */
        const size_t moved = shuttle(server, client);
        const bool server_done = server->state == TLS_ESTABLISHED;
        const bool client_done = SSL_is_init_finished(client->ssl);
        if (server_done && client_done) {
            return rounds;
        }
        if (moved == 0) {
            return -1;   /* progress stalled; handshake hung */
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Test cases
 * ------------------------------------------------------------------------- */

static void test_handshake_tls13_success(void **state)
{
    (void)state;

    char err[TLS_ERR_BUF_SIZE] = {0};
    tls_context_t *const ctx = tls_context_new(
        suite_cert.cert_path, suite_cert.key_path, err, sizeof(err));
    assert_non_null(ctx);

    tls_session_t *const server = tls_session_new(ctx);
    assert_non_null(server);
    assert_int_equal(server->state, TLS_HANDSHAKING);

    tls_test_client_t client;
    client_init(&client);

    const int rounds = pump_handshake(server, &client, 6);
    assert_in_range(rounds, 1, 6);
    assert_int_equal(server->state, TLS_ESTABLISHED);
    assert_int_equal(server->alpn_selected, TLS_ALPN_HTTP11);
    assert_true(SSL_version(server->ssl) == TLS1_3_VERSION);

    client_free(&client);
    tls_session_free(server);
    tls_context_free(ctx);
}

/**
 * Post-handshake: the server writes 1 MiB in 1 KiB chunks; the client
 * reads until it collects the same volume and compares a rolling
 * checksum. Validates the encrypt/decrypt loop and the BIO-pair
 * back-and-forth under a realistic record-boundary pattern.
 */
static void test_bulk_1mb_byte_exact(void **state)
{
    (void)state;

    char err[TLS_ERR_BUF_SIZE] = {0};
    tls_context_t *const ctx = tls_context_new(
        suite_cert.cert_path, suite_cert.key_path, err, sizeof(err));
    assert_non_null(ctx);

    tls_session_t *const server = tls_session_new(ctx);
    tls_test_client_t client;
    client_init(&client);
    assert_int_equal(pump_handshake(server, &client, 6) > 0, 1);

    enum { CHUNK = 1024, TOTAL = 1024 * 1024 };
    uint8_t *const send_buf = malloc(TOTAL);
    uint8_t *const recv_buf = malloc(TOTAL);
    assert_non_null(send_buf);
    assert_non_null(recv_buf);
    for (size_t i = 0; i < TOTAL; i++) {
        send_buf[i] = (uint8_t)(i * 31u);
    }

    size_t sent = 0, received = 0;
    char staging[TLS_BIO_RING_SIZE];

    while (received < TOTAL) {
        /* Write one chunk if we still have plaintext to push. */
        if (sent < TOTAL) {
            const size_t want = (TOTAL - sent < CHUNK) ? (TOTAL - sent) : CHUNK;
            size_t wrote = 0;
            const tls_io_result_t rc = tls_write_plaintext(
                server, (const char *)(send_buf + sent), want, &wrote);
            if (rc == TLS_IO_OK) {
                sent += wrote;
            } else {
                assert_true(rc == TLS_IO_WANT_READ || rc == TLS_IO_WANT_WRITE);
            }
        }

        /* Drain any produced ciphertext into the client's BIO. */
        while (1) {
            size_t produced = 0;
            const tls_io_result_t rc = tls_drain_ciphertext(
                server, staging, sizeof(staging), &produced);
            assert_int_equal(rc, TLS_IO_OK);
            if (produced == 0) break;
            assert_int_equal(BIO_write(client.network, staging, (int)produced),
                             (int)produced);
        }

        /* Client-side decrypt. */
        while (1) {
            size_t got = 0;
            const int rc = SSL_read_ex(client.ssl,
                                       recv_buf + received,
                                       TOTAL - received, &got);
            if (rc != 1) {
                const int code = SSL_get_error(client.ssl, rc);
                assert_true(code == SSL_ERROR_WANT_READ ||
                            code == SSL_ERROR_WANT_WRITE);
                break;
            }
            received += got;
            if (received == TOTAL) break;
        }
    }

    assert_int_equal(sent, TOTAL);
    assert_int_equal(received, TOTAL);
    assert_memory_equal(send_buf, recv_buf, TOTAL);

    free(send_buf);
    free(recv_buf);
    client_free(&client);
    tls_session_free(server);
    tls_context_free(ctx);
}

static void test_clean_shutdown(void **state)
{
    (void)state;

    char err[TLS_ERR_BUF_SIZE] = {0};
    tls_context_t *const ctx = tls_context_new(
        suite_cert.cert_path, suite_cert.key_path, err, sizeof(err));
    tls_session_t *const server = tls_session_new(ctx);
    tls_test_client_t client;
    client_init(&client);
    assert_true(pump_handshake(server, &client, 6) > 0);

    /* Server initiates close_notify. */
    const tls_io_result_t first = tls_shutdown_step(server);
    assert_true(first == TLS_IO_WANT_READ || first == TLS_IO_CLOSED);

    /* Bounce the close_notify to the client and back. */
    for (int i = 0; i < 4 && server->state != TLS_CLOSED; i++) {
        shuttle(server, &client);
        (void)SSL_shutdown(client.ssl);
        shuttle(server, &client);
        (void)tls_shutdown_step(server);
    }
    assert_int_equal(server->state, TLS_CLOSED);

    client_free(&client);
    tls_session_free(server);
    tls_context_free(ctx);
}

static void test_truncated_ciphertext_error(void **state)
{
    (void)state;

    char err[TLS_ERR_BUF_SIZE] = {0};
    tls_context_t *const ctx = tls_context_new(
        suite_cert.cert_path, suite_cert.key_path, err, sizeof(err));
    tls_session_t *const server = tls_session_new(ctx);
    tls_test_client_t client;
    client_init(&client);
    assert_true(pump_handshake(server, &client, 6) > 0);

    /* Feed a syntactically valid TLS record header followed by random
     * junk — guaranteed to fail MAC/decrypt without hanging. */
    static const uint8_t junk[64] = {
        0x17, 0x03, 0x03, 0x00, 0x30,  /* application_data, TLS 1.2, 48 bytes */
        /* 48 bytes of nonsense */
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22,
        0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xA0,
        0xB1, 0xC2, 0xD3, 0xE4, 0xF5, 0x06, 0x17, 0x28,
        0x39, 0x4A, 0x5B, 0x6C, 0x7D, 0x8E, 0x9F, 0x0A,
        0x1B, 0x2C, 0x3D, 0x4E, 0x5F, 0x60, 0x71, 0x82,
        0x93, 0xA4, 0xB5, 0xC6, 0xD7, 0xE8, 0xF9, 0x0B
    };
    size_t consumed = 0;
    (void)tls_feed_ciphertext(server, (const char *)junk, sizeof(junk),
                              &consumed);

    char buf[256];
    size_t produced = 0;
    const tls_io_result_t rc = tls_read_plaintext(server, buf, sizeof(buf),
                                                  &produced);
    assert_int_equal(rc, TLS_IO_ERROR);

    client_free(&client);
    tls_session_free(server);
    tls_context_free(ctx);
}

/**
 * If the caller keeps pushing ciphertext without ever decrypting, the
 * BIO pair fills and tls_feed_ciphertext reports TLS_IO_WANT_READ with
 * a partial consume. Draining plaintext then lets the next feed make
 * forward progress.
 */
static void test_feed_backpressure(void **state)
{
    (void)state;

    char err[TLS_ERR_BUF_SIZE] = {0};
    tls_context_t *const ctx = tls_context_new(
        suite_cert.cert_path, suite_cert.key_path, err, sizeof(err));
    tls_session_t *const server = tls_session_new(ctx);
    tls_test_client_t client;
    client_init(&client);
    assert_true(pump_handshake(server, &client, 6) > 0);

    /* Accumulate > TLS_BIO_RING_SIZE of ciphertext by interleaving the
     * client's SSL_write with drains of its network_bio — the client
     * pair is itself only 17 KiB, so we must drain between writes. */
    enum { PAYLOAD_CHUNK = 4 * 1024, GATHER_CAP = 64 * 1024 };
    uint8_t payload[PAYLOAD_CHUNK];
    memset(payload, 'A', sizeof(payload));
    char gather[GATHER_CAP];
    size_t gathered = 0;

    while (gathered < GATHER_CAP) {
        size_t n = 0;
        const int rc = SSL_write_ex(client.ssl, payload, sizeof(payload), &n);
        if (rc != 1) {
            /* Client BIO full — drain below to make space. */
        }
        while (gathered < GATHER_CAP) {
            const int got = BIO_read(client.network,
                                     gather + gathered,
                                     (int)(GATHER_CAP - gathered));
            if (got <= 0) break;
            gathered += (size_t)got;
        }
        if (gathered >= (size_t)TLS_BIO_RING_SIZE + 4096) {
            break;  /* enough to overflow the server's intake */
        }
        (void)n;
    }
    assert_true(gathered > TLS_BIO_RING_SIZE);  /* enough to overflow */

    size_t consumed = 0;
    const tls_io_result_t rc1 = tls_feed_ciphertext(
        server, (const char *)gather, gathered, &consumed);
    assert_true(rc1 == TLS_IO_OK || rc1 == TLS_IO_WANT_READ);
    assert_true(consumed < gathered);         /* partial — ring saturated */
    assert_true(consumed <= TLS_BIO_RING_SIZE);

    /* Drain plaintext, which frees space on the network_bio. */
    char plain[TLS_BIO_RING_SIZE];
    size_t produced = 0;
    (void)tls_read_plaintext(server, plain, sizeof(plain), &produced);
    assert_true(produced > 0);

    /* Now feed the remainder — space should have opened up. */
    size_t consumed_2 = 0;
    const tls_io_result_t rc2 = tls_feed_ciphertext(
        server, (const char *)(gather + consumed),
        gathered - consumed, &consumed_2);
    assert_int_equal(rc2, TLS_IO_OK);
    assert_true(consumed_2 > 0);

    client_free(&client);
    tls_session_free(server);
    tls_context_free(ctx);
}

/**
 * Fresh sessions must present "no error yet" state. Consumers rely on
 * op == TLS_OP_NONE as the presence flag at teardown — a non-zero
 * initial value would cause every clean connection to emit a spurious
 * E_NOTICE.
 */
static void test_error_info_default_zero(void **state)
{
    (void)state;

    char err[TLS_ERR_BUF_SIZE] = {0};
    tls_context_t *const ctx = tls_context_new(
        suite_cert.cert_path, suite_cert.key_path, err, sizeof(err));
    tls_session_t *const server = tls_session_new(ctx);

    const tls_error_info_t *const info = tls_session_last_error(server);
    assert_non_null(info);
    assert_int_equal(info->op, TLS_OP_NONE);
    assert_int_equal(info->ssl_err, 0);
    assert_int_equal(info->openssl_err, 0);
    assert_int_equal(info->bytes_done, 0);
    assert_int_equal(info->reason[0], '\0');

    /* tls_op_name gives a printable label even on TLS_OP_NONE. */
    assert_string_equal(tls_op_name(TLS_OP_NONE), "none");

    tls_session_free(server);
    tls_context_free(ctx);
}

/**
 * Forcing the session into TLS_IO_ERROR (garbage ciphertext, same
 * shape as test_truncated_ciphertext_error) must populate last_error
 * with TLS_OP_READ and a non-empty reason. The connection layer emits
 * exactly one E_NOTICE with these fields at teardown.
 */
static void test_error_info_recorded_on_bad_ciphertext(void **state)
{
    (void)state;

    char err[TLS_ERR_BUF_SIZE] = {0};
    tls_context_t *const ctx = tls_context_new(
        suite_cert.cert_path, suite_cert.key_path, err, sizeof(err));
    tls_session_t *const server = tls_session_new(ctx);
    tls_test_client_t client;
    client_init(&client);
    assert_true(pump_handshake(server, &client, 6) > 0);

    /* Sanity: established session still reports no error. */
    assert_int_equal(tls_session_last_error(server)->op, TLS_OP_NONE);

    static const uint8_t junk[64] = {
        0x17, 0x03, 0x03, 0x00, 0x30,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22,
        0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xA0,
        0xB1, 0xC2, 0xD3, 0xE4, 0xF5, 0x06, 0x17, 0x28,
        0x39, 0x4A, 0x5B, 0x6C, 0x7D, 0x8E, 0x9F, 0x0A,
        0x1B, 0x2C, 0x3D, 0x4E, 0x5F, 0x60, 0x71, 0x82,
        0x93, 0xA4, 0xB5, 0xC6, 0xD7, 0xE8, 0xF9, 0x0B
    };
    size_t consumed = 0;
    (void)tls_feed_ciphertext(server, (const char *)junk, sizeof(junk),
                              &consumed);

    char buf[256];
    size_t produced = 0;
    const tls_io_result_t rc = tls_read_plaintext(server, buf, sizeof(buf),
                                                  &produced);
    assert_int_equal(rc, TLS_IO_ERROR);

    const tls_error_info_t *const info = tls_session_last_error(server);
    assert_non_null(info);
    assert_int_equal(info->op, TLS_OP_READ);
    assert_true(info->reason[0] != '\0');
    /* state_at_fail captured before any transition — still ESTABLISHED. */
    assert_int_equal(info->state_at_fail, TLS_ESTABLISHED);
    /* Human label exists for the op. */
    assert_string_equal(tls_op_name(info->op), "read");

    client_free(&client);
    tls_session_free(server);
    tls_context_free(ctx);
}

/* -------------------------------------------------------------------------
 * Suite harness
 * ------------------------------------------------------------------------- */

static int group_setup(void **state)
{
    (void)state;
    if (php_test_runtime_init() != 0) {
        return -1;
    }
    if (mint_self_signed_cert(&suite_cert) != 0) {
        return -1;
    }
    return 0;
}

static int group_teardown(void **state)
{
    (void)state;
    unlink(suite_cert.cert_path);
    unlink(suite_cert.key_path);
    php_test_runtime_shutdown();
    return 0;
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_handshake_tls13_success),
        cmocka_unit_test(test_bulk_1mb_byte_exact),
        cmocka_unit_test(test_clean_shutdown),
        cmocka_unit_test(test_truncated_ciphertext_error),
        cmocka_unit_test(test_feed_backpressure),
        cmocka_unit_test(test_error_info_default_zero),
        cmocka_unit_test(test_error_info_recorded_on_bad_ciphertext),
    };
    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
