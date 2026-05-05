/*
 * Accept-Encoding parser + codec selector + MIME normaliser.
 * Pure C — no Zend, no PHP. Exercised directly by unit tests.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_HTTP_COMPRESSION

#include "compression/http_compression_negotiate.h"

#include <stddef.h>
#include <string.h>

/* ---- helpers ---------------------------------------------------------- */

/* Per-coding parse outcome. */
typedef enum { Q_UNSEEN = 0, Q_OK, Q_REJECT } q_t;

static inline char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ascii_eq_ci(const char *s, size_t slen, const char *lit)
{
    size_t llen = strlen(lit);
    if (slen != llen) return false;
    for (size_t i = 0; i < slen; i++) {
        if (ascii_lower(s[i]) != lit[i]) return false;
    }
    return true;
}

/* Is this q-value zero? Permissive: blank or malformed → non-zero (treat
 * as q=1, RFC says servers MAY ignore malformed weight). We only need
 * the binary "rejected vs accepted" decision. */
static bool q_is_zero(const char *s, size_t len)
{
    while (len > 0 && (s[0] == ' ' || s[0] == '\t')) { s++; len--; }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len == 0)         return false;
    if (s[0] != '0')      return false;
    if (len == 1)         return true;       /* "0" */
    if (s[1] != '.')      return false;      /* "0xxx" — malformed → q=1 */
    for (size_t i = 2; i < len; i++) {
        if (s[i] != '0')  return false;      /* any non-zero digit → not zero */
    }
    return true;                             /* "0.", "0.0", "0.000" */
}

/* ---- public API ------------------------------------------------------- */

void http_accept_encoding_init_default(http_accept_encoding_t *out)
{
    /* "No Accept-Encoding header" → identity only. RFC 9110 §12.5.3
     * permits any coding, but real-world clients without AE are usually
     * CLI tools / probes that may not handle gzip — and BREACH risk
     * argues for opt-in over opt-out. nginx ships the same default. */
    out->gzip_acceptable     = false;
    out->identity_acceptable = true;
}

void http_accept_encoding_parse(const char *hdr, size_t len,
                                http_accept_encoding_t *out)
{
    q_t q_gzip = Q_UNSEEN, q_identity = Q_UNSEEN, q_star = Q_UNSEEN;

    size_t i = 0;
    while (i < len) {
        /* Skip LWS and stray commas. */
        while (i < len && (hdr[i] == ' ' || hdr[i] == '\t' || hdr[i] == ',')) i++;
        if (i >= len) break;

        size_t tok_start = i;
        while (i < len && hdr[i] != ',') i++;
        size_t tok_end = i;
        while (tok_end > tok_start &&
               (hdr[tok_end - 1] == ' ' || hdr[tok_end - 1] == '\t')) {
            tok_end--;
        }
        if (tok_end == tok_start) continue;

        /* Coding name terminates at `;`, ` ` or `\t`. */
        size_t name_end = tok_start;
        while (name_end < tok_end &&
               hdr[name_end] != ';' &&
               hdr[name_end] != ' ' &&
               hdr[name_end] != '\t') {
            name_end++;
        }
        const char *name = hdr + tok_start;
        size_t      name_len = name_end - tok_start;

        bool found_q = false, qzero = false;
        size_t pi = name_end;
        while (pi < tok_end) {
            while (pi < tok_end &&
                   (hdr[pi] == ' ' || hdr[pi] == '\t' || hdr[pi] == ';')) {
                pi++;
            }
            if (pi >= tok_end) break;
            size_t param_start = pi;
            while (pi < tok_end && hdr[pi] != ';') pi++;
            size_t param_end = pi;
            while (param_end > param_start &&
                   (hdr[param_end - 1] == ' ' || hdr[param_end - 1] == '\t')) {
                param_end--;
            }

            if (param_end - param_start >= 2 &&
                ascii_lower(hdr[param_start]) == 'q' &&
                hdr[param_start + 1] == '=') {
                found_q = true;
                qzero = q_is_zero(hdr + param_start + 2,
                                  param_end - param_start - 2);
                /* Don't break — accept-ext params after q=… are legal
                 * but we ignore them; loop just falls through. */
            }
        }

        q_t outcome = (found_q && qzero) ? Q_REJECT : Q_OK;

        if      (ascii_eq_ci(name, name_len, "gzip"))     q_gzip     = outcome;
        else if (ascii_eq_ci(name, name_len, "identity")) q_identity = outcome;
        else if (name_len == 1 && name[0] == '*')         q_star     = outcome;
        /* Unknown coding: ignored. Phase-2 backends extend the if-chain. */
    }

    /* Resolution rules per RFC 9110 §12.5.3:
     *   - explicit Q_OK wins; explicit Q_REJECT wins.
     *   - unseen coding falls back to `*`: Q_OK enables, Q_REJECT excludes,
     *     Q_UNSEEN leaves it disabled.
     *   - identity has a special rule: it is acceptable by default unless
     *     the header explicitly excludes it (`identity;q=0` or `*;q=0`
     *     without a more specific identity entry). An empty header value
     *     yields gzip=Q_UNSEEN, star=Q_UNSEEN, identity=Q_UNSEEN, which
     *     resolves to "identity only" — exactly the empty-header semantic.
     */
    out->gzip_acceptable =
        (q_gzip == Q_OK) ||
        (q_gzip == Q_UNSEEN && q_star == Q_OK);

    out->identity_acceptable =
        (q_identity == Q_OK) ||
        (q_identity == Q_UNSEEN && q_star != Q_REJECT);
}

http_codec_id_t http_accept_encoding_select(const http_accept_encoding_t *ae)
{
    /* Phase-2 will preface gzip with brotli/zstd lookups in preference
     * order. The single-codec phase-1 branch keeps the type stable. */
    if (ae->gzip_acceptable) {
        return HTTP_CODEC_GZIP;
    }
    if (ae->identity_acceptable) {
        return HTTP_CODEC_IDENTITY;
    }
    return HTTP_CODEC__COUNT;
}

size_t http_compression_mime_normalize(const char *ct, size_t ct_len,
                                       char *dst, size_t dst_cap)
{
    /* Trim leading whitespace. */
    while (ct_len > 0 && (ct[0] == ' ' || ct[0] == '\t')) { ct++; ct_len--; }
    /* Stop at first `;` (parameters) or trailing whitespace. */
    size_t end = ct_len;
    for (size_t i = 0; i < ct_len; i++) {
        if (ct[i] == ';') { end = i; break; }
    }
    while (end > 0 && (ct[end - 1] == ' ' || ct[end - 1] == '\t')) end--;

    if (end == 0 || end > dst_cap) {
        return 0;
    }
    for (size_t i = 0; i < end; i++) {
        dst[i] = ascii_lower(ct[i]);
    }
    return end;
}

#endif /* HAVE_HTTP_COMPRESSION */
