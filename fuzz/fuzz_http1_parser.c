/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * libFuzzer harness for HTTP/1 request parsing (src/http1/http_parser.c).
 * Unlike the multipart harness, this one requires a live Zend runtime
 * because http_parser_execute populates an http_request_t whose fields
 * are zend_string / HashTable — all Zend allocator.
 *
 * harness_common.c brings PHP up via php_embed once; each TestOneInput
 * creates a fresh parser, feeds the fuzz bytes, destroys. Leaks between
 * invocations are caught by ASAN / leak sanitizer at process exit.
 *
 * Build:
 *     make -f fuzz/Makefile fuzz_http1_parser
 * Run:
 *     ./fuzz/fuzz_http1_parser -max_total_time=300 fuzz/corpus/http1/
 */

#include "harness_common.h"
#include "http1/http_parser.h"

#include <stdint.h>
#include <stddef.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Generous body-size limit so the parser exercises its full state
     * machine rather than tripping the limit early. Fuzzer still gets
     * to probe the limit path via the smallest inputs. */
    http1_parser_t *parser = http_parser_create(1024 * 1024);
    if (parser == NULL) {
        return 0;
    }

    /* No connection, no dispatch callback — the parser's own
     * on_message_complete handler tolerates a NULL conn; dispatch
     * just won't fire. Callbacks touching conn->server fields are
     * all guarded against NULL upstream. */
    size_t consumed = 0;
    (void)http_parser_execute(parser, (const char *)data, size, &consumed);

    http_parser_destroy(parser);
    return 0;
}
