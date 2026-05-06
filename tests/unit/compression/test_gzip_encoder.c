/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * gzip backend round-trip tests. We compress with the production
 * encoder, then inflate with the same zlib(-ng) the encoder was
 * linked against and assert byte-equality. That covers:
 *   - small one-shot bodies fitting in a single write+finish
 *   - large bodies that cross the encoder's internal output buffer
 *   - tiny output buffers that force NEED_OUTPUT looping
 *   - empty body (header + trailer only — exercises finish-only path)
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdlib.h>

#include "common/php_sapi_test.h"
#include "compression/http_encoder.h"

#ifdef HAVE_ZLIB_NG
#  include <zlib-ng.h>
#  define ZS                zng_stream
#  define ZS_INFLATE_INIT2  zng_inflateInit2
#  define ZS_INFLATE        zng_inflate
#  define ZS_INFLATE_END    zng_inflateEnd
#else
#  include <zlib.h>
#  define ZS                z_stream
#  define ZS_INFLATE_INIT2  inflateInit2
#  define ZS_INFLATE        inflate
#  define ZS_INFLATE_END    inflateEnd
#endif

/* Uncompress a gzip stream into a fresh malloc'd buffer. Caller frees. */
static unsigned char *gunzip(const unsigned char *in, size_t in_len, size_t *out_len)
{
    ZS s;
    memset(&s, 0, sizeof(s));
    /* 15+32: gzip wrapper, with auto-detect for safety. */
    assert_int_equal(ZS_INFLATE_INIT2(&s, 15 + 32), Z_OK);

    size_t cap = in_len * 4 + 64;
    unsigned char *out = malloc(cap);
    size_t produced = 0;

    s.next_in  = (void *)(uintptr_t)in;
    s.avail_in = (unsigned)in_len;

    for (;;) {
        s.next_out  = out + produced;
        s.avail_out = (unsigned)(cap - produced);
        int rc = ZS_INFLATE(&s, Z_NO_FLUSH);
        produced = cap - s.avail_out;
        if (rc == Z_STREAM_END) break;
        assert_int_equal(rc, Z_OK);
        if (s.avail_out == 0) {
            cap *= 2;
            out = realloc(out, cap);
        }
    }
    ZS_INFLATE_END(&s);
    *out_len = produced;
    return out;
}

/* Drive the encoder through write+finish with a chosen output buffer
 * size, accumulating into a fresh malloc'd byte vector. */
static unsigned char *gzip_via_encoder(const unsigned char *in, size_t in_len,
                                       size_t out_chunk, size_t *out_len)
{
    const http_encoder_vtable_t *vt = http_compression_lookup(HTTP_CODEC_GZIP);
    assert_non_null(vt);
    http_encoder_t *enc = vt->create(6);
    assert_non_null(enc);

    size_t cap = in_len + 64;
    unsigned char *out = malloc(cap);
    size_t produced = 0;
    unsigned char *chunk = malloc(out_chunk);

    /* feed input */
    size_t fed = 0;
    while (fed < in_len) {
        size_t consumed = 0, written = 0;
        http_encoder_status_t st = vt->write(enc,
            in + fed, in_len - fed, &consumed,
            chunk, out_chunk, &written);
        assert_true(st == HTTP_ENC_OK || st == HTTP_ENC_NEED_OUTPUT);
        if (produced + written > cap) {
            cap = (produced + written) * 2;
            out = realloc(out, cap);
        }
        memcpy(out + produced, chunk, written);
        produced += written;
        fed += consumed;
    }

    /* drain finish */
    for (;;) {
        size_t written = 0;
        http_encoder_status_t st = vt->finish(enc, chunk, out_chunk, &written);
        if (produced + written > cap) {
            cap = (produced + written) * 2;
            out = realloc(out, cap);
        }
        memcpy(out + produced, chunk, written);
        produced += written;
        if (st == HTTP_ENC_DONE) break;
        assert_int_equal(st, HTTP_ENC_NEED_OUTPUT);
    }

    vt->destroy(enc);
    free(chunk);
    *out_len = produced;
    return out;
}

static void roundtrip_assert(const unsigned char *in, size_t in_len, size_t out_chunk)
{
    size_t gz_len = 0;
    unsigned char *gz = gzip_via_encoder(in, in_len, out_chunk, &gz_len);
    /* gzip frame must start with magic 1f 8b. */
    assert_true(gz_len >= 2);
    assert_int_equal(gz[0], 0x1f);
    assert_int_equal(gz[1], 0x8b);

    size_t back_len = 0;
    unsigned char *back = gunzip(gz, gz_len, &back_len);
    assert_int_equal(back_len, in_len);
    if (in_len > 0) assert_memory_equal(back, in, in_len);

    free(gz);
    free(back);
}

static void test_short_text(void **state)
{
    (void)state;
    const char *msg = "Hello, gzip!";
    roundtrip_assert((const unsigned char *)msg, strlen(msg), 4096);
}

static void test_empty_body(void **state)
{
    (void)state;
    roundtrip_assert((const unsigned char *)"", 0, 4096);
}

static void test_large_body_crosses_chunks(void **state)
{
    (void)state;
    /* 256 KiB of mixed-entropy text — large enough to cross the
     * encoder's internal state several times, repetitive enough that
     * deflate has something to compress. */
    size_t n = 256 * 1024;
    unsigned char *buf = malloc(n);
    for (size_t i = 0; i < n; i++) {
        buf[i] = (unsigned char)('A' + (i * 13 + (i >> 5)) % 26);
    }
    roundtrip_assert(buf, n, 4096);
    free(buf);
}

static void test_tiny_output_buffer_forces_loop(void **state)
{
    (void)state;
    /* 16-byte output buffer is smaller than the gzip header alone, so
     * write() and finish() must loop with NEED_OUTPUT on every call. */
    const char *msg =
        "the quick brown fox jumps over the lazy dog. "
        "the quick brown fox jumps over the lazy dog. "
        "the quick brown fox jumps over the lazy dog.";
    roundtrip_assert((const unsigned char *)msg, strlen(msg), 16);
}

static void test_create_clamps_level(void **state)
{
    (void)state;
    const http_encoder_vtable_t *vt = http_compression_lookup(HTTP_CODEC_GZIP);
    /* Out-of-range levels must not crash — we clamp internally. */
    http_encoder_t *e0  = vt->create(0);   assert_non_null(e0);  vt->destroy(e0);
    http_encoder_t *e10 = vt->create(10);  assert_non_null(e10); vt->destroy(e10);
    http_encoder_t *eN  = vt->create(-1);  assert_non_null(eN);  vt->destroy(eN);
}

int main(void)
{
    if (php_test_runtime_init() != 0) return 1;

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_short_text),
        cmocka_unit_test(test_empty_body),
        cmocka_unit_test(test_large_body_crosses_chunks),
        cmocka_unit_test(test_tiny_output_buffer_forces_loop),
        cmocka_unit_test(test_create_clamps_level),
    };
    int rc = cmocka_run_group_tests(tests, NULL, NULL);

    php_test_runtime_shutdown();
    return rc;
}
