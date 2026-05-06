/*
 * Default MIME whitelist for HTTP body compression.
 *
 * Kept in a dedicated TU so that policy edits land as a focused diff
 * separate from negotiation logic. NULL-terminated, lowercase, sorted —
 * loaders rely on the sentinel; sorting helps human review only.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_HTTP_COMPRESSION

#include "compression/http_compression_defaults.h"

const char *const http_compression_default_mime_types[] = {
    "application/javascript",
    "application/json",
    "application/xml",
    "image/svg+xml",
    "text/css",
    "text/html",
    "text/javascript",
    "text/plain",
    "text/xml",
    NULL,
};

#endif /* HAVE_HTTP_COMPRESSION */
