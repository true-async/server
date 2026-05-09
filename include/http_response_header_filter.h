/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Filters for outbound response header names that the HTTP/2 and
 * HTTP/3 framed-protocol layers must not forward. The blocklist —
 * connection-specific hop-by-hop fields plus content-length, which is
 * implicit in the DATA frames — is identical for both per RFC 9113
 * §8.2.2 and RFC 9114 §4.2.
 *
 * H2 and H3 share the function so that a future addition to the list
 * doesn't have to be remembered in two places. HTTP/1 has its own
 * framing rules and uses none of this. */

#ifndef TRUE_ASYNC_HTTP_RESPONSE_HEADER_FILTER_H
#define TRUE_ASYNC_HTTP_RESPONSE_HEADER_FILTER_H

#include <stdbool.h>
#include <stddef.h>

bool http_response_header_allowed_h2h3(const char *name, size_t len);

#endif
