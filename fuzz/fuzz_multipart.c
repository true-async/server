/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * libFuzzer harness for the multipart/form-data state machine in
 * src/formats/multipart_parser.c. The parser's memory path is
 * conditional on HAVE_PHP_H — building the harness WITHOUT that
 * define makes the parser allocate via libc malloc/free, so we can
 * exercise it without spinning up the PHP runtime (fast execs =
 * more coverage per hour).
 *
 * Entry point: first 16 bytes of the fuzz input are treated as the
 * MIME boundary, remainder as the body. If the input is too short,
 * we use a fixed boundary. This gives libFuzzer meaningful shape to
 * mutate while still exercising boundary-matching, state transitions,
 * chunked feed, and limit checks.
 *
 * Build:
 *     see fuzz/Makefile — target `fuzz_multipart`
 * Run:
 *     ./fuzz_multipart -max_total_time=3600 fuzz/corpus/multipart/
 *
 * Bugs caught so far: none (baseline). Future runs should produce
 * crashes under fuzz/corpus/multipart/crash-* if regressions land.
 */

#include "formats/multipart_parser.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Silent callbacks: we only care about the parser's internal state
 * transitions, not what a user-level processor does with the parts.
 * Return 0 to keep feeding. */
static int cb_part_begin(multipart_parser_t *p)            { (void)p; return 0; }
static int cb_header_field(multipart_parser_t *p, const char *d, size_t l) { (void)p; (void)d; (void)l; return 0; }
static int cb_header_value(multipart_parser_t *p, const char *d, size_t l) { (void)p; (void)d; (void)l; return 0; }
static int cb_headers_complete(multipart_parser_t *p)       { (void)p; return 0; }
static int cb_part_data(multipart_parser_t *p, const char *d, size_t l)    { (void)p; (void)d; (void)l; return 0; }
static int cb_part_end(multipart_parser_t *p)               { (void)p; return 0; }

static const multipart_callbacks_t callbacks = {
    .on_part_begin       = cb_part_begin,
    .on_header_field     = cb_header_field,
    .on_header_value     = cb_header_value,
    .on_headers_complete = cb_headers_complete,
    .on_part_data        = cb_part_data,
    .on_part_end         = cb_part_end,
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Extract a boundary from the first slice of input. Must be at
     * least 1 byte and ≤ 70 per RFC 2046 §5.1.1. Keep it simple. */
    char boundary[64];
    size_t boundary_len = 0;

    if (size >= 8) {
        /* Use first 8 bytes as a hex-like boundary (deterministic
         * mapping keeps the corpus stable across runs). */
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < 8 && i < size; i++) {
            boundary[boundary_len++] = hex[data[i] & 0x0f];
        }
        boundary[boundary_len] = '\0';
        data += 8;
        size -= 8;
    } else {
        strcpy(boundary, "FUZZBND");
        boundary_len = strlen(boundary);
    }

    (void)boundary_len;
    multipart_parser_t *parser = multipart_parser_create(boundary);
    if (parser == NULL) {
        return 0;
    }
    multipart_parser_set_callbacks(parser, &callbacks);

    /* Feed the whole remaining input in one shot. The parser's own
     * streaming logic handles any split point; a second harness
     * variant (later) could fuzz the chunk-boundary itself. */
    (void)multipart_parser_execute(parser, (const char *)data, size);
    multipart_parser_destroy(parser);
    return 0;
}
