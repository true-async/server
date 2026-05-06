/*
 * Default policy values for HTTP body compression. Lives in its own
 * header so the response pipeline and the config-setter code share one
 * source of truth — and policy tweaks (whitelist edits, level bump) are
 * one-line diffs that don't touch the configuration plumbing.
 */
#ifndef HTTP_COMPRESSION_DEFAULTS_H
#define HTTP_COMPRESSION_DEFAULTS_H

#include <stddef.h>
#include <stdint.h>

/* Knob defaults — units match the corresponding HttpServerConfig setter. */
#define HTTP_COMPRESSION_DEFAULT_LEVEL                 6        /* gzip default */
#define HTTP_COMPRESSION_LEVEL_MIN                     1
#define HTTP_COMPRESSION_LEVEL_MAX                     9

/* Brotli quality. 4 is production-typical (~gzip-6 ratio at 5–10× speed);
 * 11 is research-quality (~50× slower than 4, marginal extra ratio). */
#define HTTP_COMPRESSION_BROTLI_DEFAULT_LEVEL          4
#define HTTP_COMPRESSION_BROTLI_LEVEL_MIN              0
#define HTTP_COMPRESSION_BROTLI_LEVEL_MAX              11

/* zstd compression level. 3 is the zstd team's own production default —
 * better ratio than gzip-6 at higher throughput. 22 is ultra mode. */
#define HTTP_COMPRESSION_ZSTD_DEFAULT_LEVEL            3
#define HTTP_COMPRESSION_ZSTD_LEVEL_MIN                1
#define HTTP_COMPRESSION_ZSTD_LEVEL_MAX                22

#define HTTP_COMPRESSION_DEFAULT_MIN_SIZE              1024u    /* below this, overhead wins */
#define HTTP_COMPRESSION_MIN_SIZE_MAX                  (16u * 1024u * 1024u)

#define HTTP_COMPRESSION_DEFAULT_REQUEST_MAX_DECOMP    (10u * 1024u * 1024u)  /* 10 MiB */

/* NULL-terminated, lowercase, sorted whitelist of MIME `type/subtype`s
 * worth gzipping by default. Matches the union of nginx `gzip_types`
 * and h2o defaults — text + structured-data only, never binary. The
 * setCompressionMimeTypes() setter REPLACES this list wholesale
 * (nginx semantics), so users who want a delta need to re-list. */
extern const char *const http_compression_default_mime_types[];

#endif /* HTTP_COMPRESSION_DEFAULTS_H */
