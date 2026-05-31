/* Unit tests for the pure-logic helpers in src/http3/http3_packet.c —
 * no server, no socket, no network. Covers:
 *   - stateless-reset token derivation (determinism + HMAC oracle);
 *   - the stateless-reset size gate (refuse <41, otherwise emit);
 *   - the sendmsg-errno -> stat-bucket mapping.
 * Linked against http3_packet.c + a one-symbol stub (http3_stubs.c). */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <cmocka.h>

#include <php.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "http3_packet.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>

/* ---- stateless-reset token: deterministic + == HMAC-SHA256(key,dcid)[0:16] ---- */
static void test_sr_token_deterministic(void **state) {
    (void)state;
    uint8_t key[32]; memset(key, 0xA5, sizeof(key));
    const uint8_t dcid[8] = {1,2,3,4,5,6,7,8};
    uint8_t t1[16], t2[16];

    http3_packet_compute_sr_token(key, dcid, sizeof(dcid), t1);
    http3_packet_compute_sr_token(key, dcid, sizeof(dcid), t2);
    assert_memory_equal(t1, t2, 16);

    uint8_t mac[32]; unsigned int maclen = sizeof(mac);
    assert_non_null(HMAC(EVP_sha256(), key, 32, dcid, sizeof(dcid), mac, &maclen));
    assert_true(maclen >= 16);
    assert_memory_equal(t1, mac, 16);
}

static void test_sr_token_differs_by_dcid(void **state) {
    (void)state;
    uint8_t key[32]; memset(key, 0x5A, sizeof(key));
    const uint8_t d1[8] = {1,1,1,1,1,1,1,1};
    const uint8_t d2[8] = {2,2,2,2,2,2,2,2};
    uint8_t t1[16], t2[16];

    http3_packet_compute_sr_token(key, d1, sizeof(d1), t1);
    http3_packet_compute_sr_token(key, d2, sizeof(d2), t2);
    assert_memory_not_equal(t1, t2, 16);
}

/* ---- stateless-reset size gate: refuse <41, emit otherwise ---- */
static void test_stateless_reset_size_gate(void **state) {
    (void)state;
    uint8_t key[32]; memset(key, 0x33, sizeof(key));
    const uint8_t dcid[8] = {9,8,7,6,5,4,3,2};

    struct sockaddr_in peer; memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(443);
    peer.sin_addr.s_addr = htonl(0x7f000001);

    /* The listener ptr only reaches the stub send_packet, which ignores it. */
    http3_listener_t *fake = (http3_listener_t *)0x1;

    assert_false(http3_packet_send_stateless_reset(
        fake, key, dcid, sizeof(dcid), 40,
        (const struct sockaddr *)&peer, sizeof(peer)));   /* < 41 -> refuse */
    assert_true(http3_packet_send_stateless_reset(
        fake, key, dcid, sizeof(dcid), 1500,
        (const struct sockaddr *)&peer, sizeof(peer)));    /* large -> emit */
}

/* ---- sendmsg errno -> stat bucket ---- */
static void test_account_send_error_buckets(void **state) {
    (void)state;
    http3_packet_stats_t st; memset(&st, 0, sizeof(st));

    http3_packet_account_send_error(&st, EAGAIN);
    http3_packet_account_send_error(&st, EMSGSIZE);
    http3_packet_account_send_error(&st, EHOSTUNREACH);
    http3_packet_account_send_error(&st, EPIPE);    /* uncategorised -> other */

    assert_int_equal(st.quic_send_eagain,      1);
    assert_int_equal(st.quic_send_emsgsize,    1);
    assert_int_equal(st.quic_send_unreach,     1);
    assert_int_equal(st.quic_send_other_error, 1);

    http3_packet_account_send_error(NULL, EAGAIN);   /* NULL-safe, no crash */
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_sr_token_deterministic),
        cmocka_unit_test(test_sr_token_differs_by_dcid),
        cmocka_unit_test(test_stateless_reset_size_gate),
        cmocka_unit_test(test_account_send_error_buckets),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
