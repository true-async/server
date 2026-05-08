/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "static/http_static_etag.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static inline uint64_t mtime_ns_from_stat(const struct stat *st)
{
#if defined(__APPLE__)
    return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ULL
         + (uint64_t)st->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (uint64_t)st->st_mtime * 1000000000ULL;
#else
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ULL
         + (uint64_t)st->st_mtim.tv_nsec;
#endif
}

/* Hand-rolled %02d / %04d / %016x writers. snprintf via format_converter
 * runs ~5-7% of total CPU under load — printf machinery (format-string
 * scan, va_list dispatch) on every request is gross overkill for these
 * fixed-width unsigned integers. Direct table-and-divmod is ~1 order
 * of magnitude cheaper and matches the static buffer's exact width. */

static const char hex_lower[16] = {
    '0','1','2','3','4','5','6','7',
    '8','9','a','b','c','d','e','f'
};

static inline void put_u8_2digit(char *out, unsigned v)
{
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + (v % 10));
}

static inline void put_u16_4digit(char *out, unsigned v)
{
    out[0] = (char)('0' + (v / 1000) % 10);
    out[1] = (char)('0' + (v / 100)  % 10);
    out[2] = (char)('0' + (v / 10)   % 10);
    out[3] = (char)('0' + (v        ) % 10);
}

static inline void put_u64_16hex(char *out, uint64_t v)
{
    for (int i = 15; i >= 0; --i) {
        out[i]  = hex_lower[v & 0xF];
        v     >>= 4;
    }
}

void http_static_etag_format(const struct stat *st,
                             char buf[HTTP_STATIC_ETAG_BUF_LEN])
{
    /* Single 64-bit mix — SHA/MD5 cost more per request than the
     * conditional GET saves. See docs/PLAN_STATIC_HANDLER.md §4. */
    const uint64_t mtime_ns = mtime_ns_from_stat(st);
    const uint64_t size     = (uint64_t)st->st_size;
    const uint64_t ino      = (uint64_t)st->st_ino;
    const uint64_t mix      = mtime_ns ^ (size << 17) ^ ino;

    /* Layout: W / " <16-hex> " — 20 bytes + NUL. */
    buf[0] = 'W';
    buf[1] = '/';
    buf[2] = '"';
    put_u64_16hex(buf + 3, mix);
    buf[19] = '"';
    buf[20] = '\0';
    ZEND_STATIC_ASSERT(HTTP_STATIC_ETAG_LEN == 20,
                       "etag length must match hand-format");
}

void http_static_format_http_date(time_t t, char buf[HTTP_STATIC_DATE_BUF_LEN])
{
    struct tm tm;
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    /* Hard-coded English month/day names — strftime would honour
     * LC_TIME and could produce non-canonical strings on hosts that
     * forgot to set the locale. RFC 7231 IMF-fixdate is fixed grammar. */
    static const char day_names[7][3]    = {
        {'S','u','n'}, {'M','o','n'}, {'T','u','e'},
        {'W','e','d'}, {'T','h','u'}, {'F','r','i'}, {'S','a','t'} };
    static const char month_names[12][3] = {
        {'J','a','n'}, {'F','e','b'}, {'M','a','r'}, {'A','p','r'},
        {'M','a','y'}, {'J','u','n'}, {'J','u','l'}, {'A','u','g'},
        {'S','e','p'}, {'O','c','t'}, {'N','o','v'}, {'D','e','c'} };

    /* mtime values past year 9999 are filesystem tampering, not real;
     * clamp to keep the fixed-width buffer honest. */
    int year = tm.tm_year + 1900;
    if (UNEXPECTED(year < 0 || year > 9999)) {
        year = 9999;
    }

    /* Layout: "Day, DD Mon YYYY HH:MM:SS GMT" — 29 bytes + NUL.
     * Indices: 0..2 day, 3..4 ", ", 5..6 DD, 7 ' ', 8..10 Mon,
     * 11 ' ', 12..15 YYYY, 16 ' ', 17..18 HH, 19 ':', 20..21 MM,
     * 22 ':', 23..24 SS, 25..28 " GMT", 29 NUL. */
    const char *const day = day_names[tm.tm_wday & 7];
    const char *const mon = month_names[tm.tm_mon % 12];
    buf[0]  = day[0]; buf[1]  = day[1]; buf[2]  = day[2];
    buf[3]  = ',';    buf[4]  = ' ';
    put_u8_2digit(buf + 5, (unsigned)tm.tm_mday);
    buf[7]  = ' ';
    buf[8]  = mon[0]; buf[9]  = mon[1]; buf[10] = mon[2];
    buf[11] = ' ';
    put_u16_4digit(buf + 12, (unsigned)year);
    buf[16] = ' ';
    put_u8_2digit(buf + 17, (unsigned)tm.tm_hour);
    buf[19] = ':';
    put_u8_2digit(buf + 20, (unsigned)tm.tm_min);
    buf[22] = ':';
    put_u8_2digit(buf + 23, (unsigned)tm.tm_sec);
    buf[25] = ' '; buf[26] = 'G'; buf[27] = 'M'; buf[28] = 'T';
    buf[29] = '\0';
    ZEND_STATIC_ASSERT(HTTP_STATIC_DATE_LEN == 29,
                       "date length must match hand-format");
}

static inline void trim_ws(const char **start, size_t *len)
{
    while (*len > 0 && ((*start)[0] == ' ' || (*start)[0] == '\t')) {
        (*start)++;
        (*len)--;
    }
    while (*len > 0 && ((*start)[*len - 1] == ' ' || (*start)[*len - 1] == '\t')) {
        (*len)--;
    }
}

/* Strip a leading W/ (case-sensitive per RFC 9110 §8.8.3) so a weak
 * tag from the server compares equal to a weak tag echoed by the
 * client. */
static inline void strip_weak_prefix(const char **s, size_t *len)
{
    if (*len >= 2 && (*s)[0] == 'W' && (*s)[1] == '/') {
        *s   += 2;
        *len -= 2;
    }
}

/* RFC 9110 §13.1.2: comma-separated entries, weak-equal comparison,
 * "*" matches any existing resource. */
static bool match_if_none_match(const char *header, size_t header_len,
                                const char *etag, size_t etag_len)
{
    /* Strip W/ on the etag side too so a strong tag compared to a
     * weak echo matches "weak-equal" as RFC 9110 §13.1.2 prescribes
     * for If-None-Match. */
    strip_weak_prefix(&etag, &etag_len);

    size_t i = 0;
    while (i < header_len) {
        size_t start = i;
        while (i < header_len && header[i] != ',') i++;
        size_t entry_len = i - start;
        const char *entry = header + start;
        trim_ws(&entry, &entry_len);

        if (entry_len == 1 && entry[0] == '*') {
            return true;
        }

        const char *cmp = entry;
        size_t      cmp_len = entry_len;
        strip_weak_prefix(&cmp, &cmp_len);

        if (cmp_len == etag_len && memcmp(cmp, etag, etag_len) == 0) {
            return true;
        }

        if (i < header_len) i++; /* skip the comma */
    }
    return false;
}

/* RFC 9110 §5.6.7 accepts three formats; try in order of frequency. */
static time_t parse_http_date(const char *src, size_t src_len)
{
    char buf[64];
    if (src_len == 0 || src_len >= sizeof(buf)) {
        return (time_t)-1;
    }
    memcpy(buf, src, src_len);
    buf[src_len] = '\0';

    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    /* IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT" */
    if (strptime(buf, "%a, %d %b %Y %H:%M:%S GMT", &tm) != NULL) {
        return timegm(&tm);
    }
    memset(&tm, 0, sizeof(tm));
    /* RFC 850:    "Sunday, 06-Nov-94 08:49:37 GMT" */
    if (strptime(buf, "%A, %d-%b-%y %H:%M:%S GMT", &tm) != NULL) {
        return timegm(&tm);
    }
    memset(&tm, 0, sizeof(tm));
    /* asctime:    "Sun Nov  6 08:49:37 1994" */
    if (strptime(buf, "%a %b %e %H:%M:%S %Y", &tm) != NULL) {
        return timegm(&tm);
    }
    return (time_t)-1;
}

bool http_static_conditional_match(const char *if_none_match,
                                   size_t if_none_match_len,
                                   const char *if_modified_since,
                                   size_t if_modified_since_len,
                                   const char *etag, size_t etag_len,
                                   time_t last_modified)
{
    /* RFC 9110 §13.1.2: If-None-Match takes precedence; If-Modified-
     * Since is consulted only when If-None-Match is absent. */
    if (if_none_match_len > 0 && if_none_match != NULL) {
        return match_if_none_match(if_none_match, if_none_match_len,
                                   etag, etag_len);
    }
    if (if_modified_since_len > 0 && if_modified_since != NULL) {
        const time_t since = parse_http_date(if_modified_since,
                                             if_modified_since_len);
        if (since == (time_t)-1) {
            return false;
        }
        /* "Not modified" iff last_modified <= since. */
        return last_modified <= since;
    }
    return false;
}
