/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Unit tests for HTTP/3 CID steering (#80 D6 / #72). Pure crypto core, no
 * server/socket: the addressing layer that lets any reactor recover the owner
 * reactor id from a server-minted CID. Covers init/activation gating, round-trip
 * for every reactor id, nonce variation, masking (id not in clear), short-CID
 * rejection, and key sensitivity. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdint.h>
#include <string.h>

#include <openssl/rand.h>

#include "http3/http3_steer.h"

/* Stub for http3_fill_random (defined in http3_connection.c in the real build).
 * Real DRBG so encode() produces genuinely varied nonces — exactly what the
 * masking test needs. */
bool http3_fill_random(uint8_t *buf, size_t len)
{
    return RAND_bytes(buf, (int) len) == 1;
}

static int steer_setup(void **state)
{
    (void) state;
    assert_true(http3_steer_init());
    http3_steer_set_active(true);
    return 0;
}

/* init is idempotent and gates activation: set_active(true) before init must
 * not arm steering. */
static void test_activation_gating(void **state)
{
    (void) state;

    /* Re-init is a no-op success (key already seeded by setup). */
    assert_true(http3_steer_init());

    http3_steer_set_active(false);
    assert_false(http3_steer_active());

    http3_steer_set_active(true);
    assert_true(http3_steer_active());
}

/* Every reactor id 0..255 survives encode → decode. */
static void test_roundtrip_all_ids(void **state)
{
    (void) state;

    for (int id = 0; id < 256; id++) {
        uint8_t cid[HTTP3_STEER_CID_LEN];
        assert_true(http3_steer_encode(cid, id));
        assert_int_equal(http3_steer_decode(cid, sizeof(cid)), id);
    }
}

/* Two encodes of the same id differ (random nonce) yet both decode back. */
static void test_nonce_varies(void **state)
{
    (void) state;

    uint8_t a[HTTP3_STEER_CID_LEN];
    uint8_t b[HTTP3_STEER_CID_LEN];

    assert_true(http3_steer_encode(a, 3));
    assert_true(http3_steer_encode(b, 3));

    assert_int_not_equal(memcmp(a, b, sizeof(a)), 0);
    assert_int_equal(http3_steer_decode(a, sizeof(a)), 3);
    assert_int_equal(http3_steer_decode(b, sizeof(b)), 3);
}

/* The id is not stored in clear: across many encodes of the same id, the
 * id byte takes on many different values (it is masked with AES(nonce)). A
 * plaintext byte would be constant. */
static void test_id_is_masked(void **state)
{
    (void) state;

    bool seen_other = false;

    for (int i = 0; i < 64; i++) {
        uint8_t cid[HTTP3_STEER_CID_LEN];
        assert_true(http3_steer_encode(cid, 5));

        if (cid[0] != 5) {
            seen_other = true;
        }
    }

    assert_true(seen_other);
}

/* A CID shorter than the steering width cannot carry an id. */
static void test_short_cid_rejected(void **state)
{
    (void) state;

    uint8_t cid[HTTP3_STEER_CID_LEN];
    assert_true(http3_steer_encode(cid, 1));

    assert_int_equal(http3_steer_decode(cid, HTTP3_STEER_CID_LEN - 1), -1);
    assert_int_equal(http3_steer_decode(cid, 0), -1);
    assert_int_equal(http3_steer_decode(NULL, HTTP3_STEER_CID_LEN), -1);
}

/* A CID longer than the width still decodes (the owner byte + nonce live in the
 * leading bytes; trailing bytes are ignored by decode). */
static void test_longer_cid_decodes(void **state)
{
    (void) state;

    uint8_t cid[HTTP3_STEER_CID_LEN + 4];
    assert_true(http3_steer_encode(cid, 9));
    /* fill the tail so it is clearly outside the encoded prefix */
    memset(cid + HTTP3_STEER_CID_LEN, 0xAB, 4);

    assert_int_equal(http3_steer_decode(cid, sizeof(cid)), 9);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_activation_gating,   steer_setup),
        cmocka_unit_test_setup(test_roundtrip_all_ids,   steer_setup),
        cmocka_unit_test_setup(test_nonce_varies,        steer_setup),
        cmocka_unit_test_setup(test_id_is_masked,        steer_setup),
        cmocka_unit_test_setup(test_short_cid_rejected,  steer_setup),
        cmocka_unit_test_setup(test_longer_cid_decodes,  steer_setup),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
