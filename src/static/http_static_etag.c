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

void http_static_etag_format(const struct stat *st,
                             char buf[HTTP_STATIC_ETAG_BUF_LEN])
{
    /* Single 64-bit mix — SHA/MD5 cost more per request than the
     * conditional GET saves. See docs/PLAN_STATIC_HANDLER.md §4. */
    const uint64_t mtime_ns = mtime_ns_from_stat(st);
    const uint64_t size     = (uint64_t)st->st_size;
    const uint64_t ino      = (uint64_t)st->st_ino;
    const uint64_t mix      = mtime_ns ^ (size << 17) ^ ino;

    const int written = snprintf(buf, HTTP_STATIC_ETAG_BUF_LEN,
                                 "W/\"%016" PRIx64 "\"", mix);
    ZEND_ASSERT(written == HTTP_STATIC_ETAG_LEN);
    (void)written;
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
    static const char *const day_names[]   = {
        "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *const month_names[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec" };

    snprintf(buf, HTTP_STATIC_DATE_BUF_LEN,
             "%s, %02d %s %04d %02d:%02d:%02d GMT",
             day_names[tm.tm_wday & 7],
             tm.tm_mday,
             month_names[tm.tm_mon % 12],
             tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
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
