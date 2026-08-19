/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * Mid-stream flush for the Brotli and zstd backends (#170). Both cases
 * ask the same question the gzip tests ask of `gz_flush`: after write()
 * followed by flush(), does the codec's own decoder read the bytes fed
 * so far, while the stream is still open. Each case runs twice, once
 * with room to spare and once with an 8-byte output buffer that forces
 * flush() through its NEED_OUTPUT branch.
 *
 * gzip lives in test_gzip_encoder.c: it decodes with the same zlib the
 * encoder is linked against, which this file cannot share.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "common/php_sapi_test.h"
#include "compression/http_encoder.h"

#ifdef HAVE_HTTP_ZSTD
#include <zstd.h>
#endif

#ifdef HAVE_HTTP_BROTLI
#include <brotli/decode.h>
#endif

static const char FIRST[]  = "first chunk, flushed while the stream is still open. ";
static const char SECOND[] = "second chunk, written after the flush.";

/* Encoded output of one run, plus the length at which the first flush
 * ended — the decoder is offered exactly that prefix. */
typedef struct {
    unsigned char *bytes;
    size_t         len;
    size_t         prefix_len;
    size_t         flush_need_output;
} encoded_t;

static encoded_t encode_with_flush(http_codec_id_t codec, int level, size_t out_chunk)
{
    const http_encoder_vtable_t *vt = http_compression_lookup(codec);
    assert_non_null(vt);
    assert_non_null(vt->flush);

    http_encoder_t *enc = vt->create(level);
    assert_non_null(enc);

    const size_t   cap   = 4096;              /* both chunks encode well under this */
    encoded_t      r     = { malloc(cap), 0, 0, 0 };
    unsigned char *chunk = malloc(out_chunk);

    const char *parts[2] = { FIRST, SECOND };

    for (int i = 0; i < 2; i++) {
        const unsigned char *in = (const unsigned char *)parts[i];
        const size_t in_len = strlen(parts[i]);
        size_t fed = 0;

        while (fed < in_len) {
            size_t consumed = 0, written = 0;
            const http_encoder_status_t st = vt->write(enc,
                in + fed, in_len - fed, &consumed, chunk, out_chunk, &written);
            assert_true(st == HTTP_ENC_OK || st == HTTP_ENC_NEED_OUTPUT);
            assert_true(r.len + written <= cap);
            memcpy(r.bytes + r.len, chunk, written);
            r.len += written;
            fed   += consumed;
        }

        if (i == 0) {
            for (;;) {
                size_t written = 0;
                const http_encoder_status_t st = vt->flush(enc, chunk, out_chunk, &written);
                assert_true(r.len + written <= cap);
                memcpy(r.bytes + r.len, chunk, written);
                r.len += written;

                if (st == HTTP_ENC_DONE) break;
                assert_int_equal(st, HTTP_ENC_NEED_OUTPUT);
                r.flush_need_output++;
            }

            r.prefix_len = r.len;
            assert_true(r.prefix_len > 0);
        }
    }

    for (;;) {
        size_t written = 0;
        const http_encoder_status_t st = vt->finish(enc, chunk, out_chunk, &written);
        assert_true(r.len + written <= cap);
        memcpy(r.bytes + r.len, chunk, written);
        r.len += written;

        if (st == HTTP_ENC_DONE) break;
        assert_int_equal(st, HTTP_ENC_NEED_OUTPUT);
    }

    vt->destroy(enc);
    free(chunk);
    return r;
}

static void assert_reads_back(const char *got, size_t got_len, const char *expected)
{
    assert_int_equal(got_len, strlen(expected));
    assert_memory_equal(got, expected, got_len);
}

#ifdef HAVE_HTTP_ZSTD
/* Decode as much as the frame prefix allows. An unfinished frame is not
 * an error for ZSTD_decompressStream: it stops when the input runs out. */
static size_t zstd_decode(const unsigned char *in, size_t in_len, char *out, size_t out_cap)
{
    ZSTD_DStream *ds = ZSTD_createDStream();
    assert_non_null(ds);
    ZSTD_inBuffer  ib = { .src = in,  .size = in_len,  .pos = 0 };
    ZSTD_outBuffer ob = { .dst = out, .size = out_cap, .pos = 0 };

    while (ib.pos < ib.size) {
        const size_t rc = ZSTD_decompressStream(ds, &ob, &ib);
        assert_false(ZSTD_isError(rc));

        if (rc == 0) break;                   /* frame complete */
    }

    ZSTD_freeDStream(ds);
    return ob.pos;
}

static void zstd_flush_case(size_t out_chunk, bool expect_flush_loop)
{
    encoded_t r = encode_with_flush(HTTP_CODEC_ZSTD, 3, out_chunk);

    char plain[4096];
    const size_t plain_len = zstd_decode(r.bytes, r.prefix_len, plain, sizeof(plain));
    assert_reads_back(plain, plain_len, FIRST);

    char whole[4096];
    const size_t whole_len = zstd_decode(r.bytes, r.len, whole, sizeof(whole));
    assert_int_equal(whole_len, strlen(FIRST) + strlen(SECOND));
    assert_memory_equal(whole, FIRST, strlen(FIRST));
    assert_memory_equal(whole + strlen(FIRST), SECOND, strlen(SECOND));

    if (expect_flush_loop) {
        assert_true(r.flush_need_output > 0);
    }

    free(r.bytes);
}

static void test_zstd_flush_exposes_first_chunk(void **state)
{
    (void)state;
    zstd_flush_case(4096, false);
}

static void test_zstd_flush_tiny_output_buffer(void **state)
{
    (void)state;
    zstd_flush_case(8, true);
}
#endif /* HAVE_HTTP_ZSTD */

#ifdef HAVE_HTTP_BROTLI
static size_t brotli_decode(const unsigned char *in, size_t in_len, char *out, size_t out_cap)
{
    BrotliDecoderState *ds = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    assert_non_null(ds);

    size_t         avail_in  = in_len;
    const uint8_t *next_in   = (const uint8_t *)in;
    size_t         avail_out = out_cap;
    uint8_t       *next_out  = (uint8_t *)out;
    size_t         total     = 0;

    const BrotliDecoderResult rc = BrotliDecoderDecompressStream(
        ds, &avail_in, &next_in, &avail_out, &next_out, &total);
    /* NEEDS_MORE_INPUT is the expected answer for a stream that has only
     * been flushed, not finished. */
    assert_true(rc == BROTLI_DECODER_RESULT_SUCCESS
                || rc == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT);

    const size_t produced = out_cap - avail_out;
    BrotliDecoderDestroyInstance(ds);
    return produced;
}

static void brotli_flush_case(size_t out_chunk, bool expect_flush_loop)
{
    encoded_t r = encode_with_flush(HTTP_CODEC_BROTLI, 4, out_chunk);

    char plain[4096];
    const size_t plain_len = brotli_decode(r.bytes, r.prefix_len, plain, sizeof(plain));
    assert_reads_back(plain, plain_len, FIRST);

    char whole[4096];
    const size_t whole_len = brotli_decode(r.bytes, r.len, whole, sizeof(whole));
    assert_int_equal(whole_len, strlen(FIRST) + strlen(SECOND));
    assert_memory_equal(whole, FIRST, strlen(FIRST));
    assert_memory_equal(whole + strlen(FIRST), SECOND, strlen(SECOND));

    if (expect_flush_loop) {
        assert_true(r.flush_need_output > 0);
    }

    free(r.bytes);
}

static void test_brotli_flush_exposes_first_chunk(void **state)
{
    (void)state;
    brotli_flush_case(4096, false);
}

static void test_brotli_flush_tiny_output_buffer(void **state)
{
    (void)state;
    brotli_flush_case(8, true);
}
#endif /* HAVE_HTTP_BROTLI */

int main(void)
{
#if !defined(HAVE_HTTP_ZSTD) && !defined(HAVE_HTTP_BROTLI)
    return 0;                                 /* neither codec compiled in */
#else
    if (php_test_runtime_init() != 0) return 1;

    const struct CMUnitTest tests[] = {
#ifdef HAVE_HTTP_ZSTD
        cmocka_unit_test(test_zstd_flush_exposes_first_chunk),
        cmocka_unit_test(test_zstd_flush_tiny_output_buffer),
#endif
#ifdef HAVE_HTTP_BROTLI
        cmocka_unit_test(test_brotli_flush_exposes_first_chunk),
        cmocka_unit_test(test_brotli_flush_tiny_output_buffer),
#endif
    };
    int rc = cmocka_run_group_tests(tests, NULL, NULL);

    php_test_runtime_shutdown();
    return rc;
#endif
}
