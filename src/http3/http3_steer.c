/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  HTTP/3 CID steering core (#80 D6 / #72). See http3_steer.h.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "http3_steer.h"

#include <string.h>
#include <openssl/evp.h>

/* Compiler TLS keyword without pulling in php.h (this TU stays runtime-free,
 * so ZEND_TLS is not available here). */
#ifdef _MSC_VER
# define STEER_TLS __declspec(thread)
#else
# define STEER_TLS __thread
#endif

/* Secure random bytes (defined in http3_connection.c). Forward-declared here
 * rather than pulling in the heavy http3_internal.h (php.h + ngtcp2 + nghttp3)
 * so this TU stays runtime-free and unit-testable in isolation. */
bool http3_fill_random(uint8_t *buf, size_t len);

/* Per-process steering secret. Seeded once via the OpenSSL DRBG in
 * http3_steer_init; read-only afterwards so every reactor thread can derive
 * the keystream byte without locking. */
static uint8_t g_steer_key[16];
static bool    g_steer_ready  = false;
static bool    g_steer_active = false;

/* One AES-128-ECB block as a PRF: CID nonce in, keystream byte out. The
 * cipher context is thread-local and reused, not new/free'd per call. */
static bool http3_steer_block(const uint8_t in[16], uint8_t out[16])
{
    static STEER_TLS EVP_CIPHER_CTX *ctx = NULL;

    if (ctx == NULL) {
        ctx = EVP_CIPHER_CTX_new();

        if (ctx == NULL) {
            return false;
        }
    } else {
        EVP_CIPHER_CTX_reset(ctx);
    }

    int outl = 0;

    return EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, g_steer_key, NULL) == 1
        && EVP_CIPHER_CTX_set_padding(ctx, 0) == 1
        && EVP_EncryptUpdate(ctx, out, &outl, in, 16) == 1
        && outl == 16;
}

/* Keystream byte for a 7-byte nonce: AES(key, nonce || zero-pad)[0]. */
static bool http3_steer_mask(const uint8_t *nonce, uint8_t *out_mask)
{
    uint8_t block[16] = {0};
    uint8_t cipher[16];

    memcpy(block, nonce, HTTP3_STEER_CID_LEN - 1);

    if (!http3_steer_block(block, cipher)) {
        return false;
    }

    *out_mask = cipher[0];

    return true;
}

bool http3_steer_init(void)
{
    if (g_steer_ready) {
        return true;
    }

    if (!http3_fill_random(g_steer_key, sizeof(g_steer_key))) {
        return false;
    }

    g_steer_ready = true;

    return true;
}

void http3_steer_set_active(const bool active)
{
    g_steer_active = active && g_steer_ready;
}

bool http3_steer_active(void)
{
    return g_steer_active;
}

bool http3_steer_encode(uint8_t *cid, const int reactor_id)
{
    if (cid == NULL || !g_steer_ready) {
        return false;
    }

    /* Random nonce in [1..], id byte masked at [0]. */
    if (!http3_fill_random(cid + 1, HTTP3_STEER_CID_LEN - 1)) {
        return false;
    }

    uint8_t mask = 0;

    if (!http3_steer_mask(cid + 1, &mask)) {
        return false;
    }

    cid[0] = (uint8_t)((uint8_t)reactor_id ^ mask);

    return true;
}

int http3_steer_decode(const uint8_t *cid, const size_t cidlen)
{
    if (cid == NULL || cidlen < HTTP3_STEER_CID_LEN) {
        return -1;
    }

    uint8_t mask = 0;

    if (!http3_steer_mask(cid + 1, &mask)) {
        return -1;
    }

    return (int)(uint8_t)(cid[0] ^ mask);
}
