/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * libFuzzer harness for HTTP/2 wire feeding (src/http2/http2_session.c).
 * Creates a server-side session in offline-test mode (conn=NULL) and
 * feeds fuzz bytes through http2_session_feed — this drives every
 * callback we register (begin_frame, begin_headers, header, data_chunk,
 * frame_recv, stream_close, invalid_frame) against a raw mutated
 * byte stream.
 *
 * Seed corpus includes valid h2 preface + SETTINGS, so the fuzzer
 * starts from a well-formed session and mutates frames.
 *
 * Requires libphp (http2_stream uses zend_string / HashTable) and
 * libnghttp2.
 */

#include "harness_common.h"
#include "http2/http2_session.h"

#include <stdint.h>
#include <stddef.h>

static void noop_request_ready(struct http_request_t *req, uint32_t sid, void *ud)
{
    (void)req; (void)sid; (void)ud;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    http2_session_t *session = http2_session_new(NULL, noop_request_ready, NULL);
    if (session == NULL) {
        return 0;
    }

    size_t consumed = 0;
    (void)http2_session_feed(session, (const char *)data, size, &consumed);

    http2_session_free(session);
    return 0;
}
