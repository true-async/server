/*
 * Accept-Encoding parsing and codec selection — pure C, no Zend deps,
 * so unit tests can exercise the state machine without a PHP runtime.
 *
 * Phase-1 surface: distinguish gzip from identity, with q-values, the
 * `*` wildcard, and `identity;q=0` semantics per RFC 9110 §12.5.3.
 * Phase-2 codecs (Brotli, zstd) extend the result struct in place; the
 * select() function walks them in preference order.
 */
#ifndef HTTP_COMPRESSION_NEGOTIATE_H
#define HTTP_COMPRESSION_NEGOTIATE_H

#include <stddef.h>
#include <stdbool.h>

#include "compression/http_encoder.h"

typedef struct {
    bool gzip_acceptable;
    bool identity_acceptable;
} http_accept_encoding_t;

/* Initialise to the "no Accept-Encoding header was sent" default. We
 * deliberately resolve this to identity-only (gzip rejected) rather
 * than RFC 9110 §12.5.3's strict "any coding acceptable" — see the
 * impl comment for the rationale (BREACH-safe-by-default + matching
 * nginx). Distinct from parsing an empty header value, which also
 * resolves to identity-only via parse() but for a different RFC reason. */
void http_accept_encoding_init_default(http_accept_encoding_t *out);

/* Parse a single Accept-Encoding header value. Multi-value headers
 * (RFC: multiple Accept-Encoding lines collapse with `,`) should be
 * concatenated by the caller before calling this. Tolerant of LWS,
 * unknown codings (ignored), malformed q values (treated as q=1).
 * len=0 → only identity acceptable (empty header semantics). */
void http_accept_encoding_parse(const char *hdr, size_t len,
                                http_accept_encoding_t *out);

/* Pick the best codec given the parsed prefs and what we have built in.
 *   HTTP_CODEC_GZIP       — encode with gzip
 *   HTTP_CODEC_IDENTITY   — send raw
 *   HTTP_CODEC__COUNT     — sentinel: client refuses every coding we
 *                           can offer (incl. identity). Caller should
 *                           respond 406 Not Acceptable. */
http_codec_id_t http_accept_encoding_select(const http_accept_encoding_t *ae);

/* Strip MIME parameters (`;…`), trim, lowercase. Writes up to `dst_cap`
 * bytes (no trailing NUL) and returns the normalised length, or 0 if
 * the input normalises to empty. dst may equal src for in-place use.
 * Caller-sized buffer: passing dst_cap >= ct_len always suffices. */
size_t http_compression_mime_normalize(const char *ct, size_t ct_len,
                                       char *dst, size_t dst_cap);

#endif /* HTTP_COMPRESSION_NEGOTIATE_H */
