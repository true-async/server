/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "http_date.h"

#include <string.h>
#include <time.h>

/* Hand-rolled %02d / %04d writers. snprintf via format_converter
 * runs ~5-7% of total CPU under load — printf machinery (format-string
 * scan, va_list dispatch) on every request is gross overkill for these
 * fixed-width unsigned integers. Direct table-and-divmod is ~1 order
 * of magnitude cheaper and matches the static buffer's exact width. */

static inline void put_u8_2digit(char *out, unsigned v)
{
	out[0] = (char)('0' + (v / 10) % 10);
	out[1] = (char)('0' + (v % 10));
}

static inline void put_u16_4digit(char *out, unsigned v)
{
	out[0] = (char)('0' + (v / 1000) % 10);
	out[1] = (char)('0' + (v / 100) % 10);
	out[2] = (char)('0' + (v / 10) % 10);
	out[3] = (char)('0' + (v) % 10);
}

void http_date_format_imf(time_t t, char buf[HTTP_DATE_BUF_LEN])
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
	static const char day_names[7][3] = {{'S', 'u', 'n'}, {'M', 'o', 'n'}, {'T', 'u', 'e'},
										 {'W', 'e', 'd'}, {'T', 'h', 'u'}, {'F', 'r', 'i'},
										 {'S', 'a', 't'}};
	static const char month_names[12][3] = {{'J', 'a', 'n'}, {'F', 'e', 'b'}, {'M', 'a', 'r'},
											{'A', 'p', 'r'}, {'M', 'a', 'y'}, {'J', 'u', 'n'},
											{'J', 'u', 'l'}, {'A', 'u', 'g'}, {'S', 'e', 'p'},
											{'O', 'c', 't'}, {'N', 'o', 'v'}, {'D', 'e', 'c'}};

	/* mtime values past year 9999 are filesystem tampering, not real;
	 * clamp to keep the fixed-width buffer honest. */
	int year = tm.tm_year + 1900;

	if (UNEXPECTED(year < 0 || year > 9999)) {
		year = 9999;
	}

	/* Layout: "Day, DD Mon YYYY HH:MM:SS GMT" — 29 bytes + NUL. */
	const char *day = day_names[tm.tm_wday & 7];
	const char *mon = month_names[tm.tm_mon % 12];
	buf[0] = day[0];
	buf[1] = day[1];
	buf[2] = day[2];
	buf[3] = ',';
	buf[4] = ' ';
	put_u8_2digit(buf + 5, (unsigned)tm.tm_mday);
	buf[7] = ' ';
	buf[8] = mon[0];
	buf[9] = mon[1];
	buf[10] = mon[2];
	buf[11] = ' ';
	put_u16_4digit(buf + 12, (unsigned)year);
	buf[16] = ' ';
	put_u8_2digit(buf + 17, (unsigned)tm.tm_hour);
	buf[19] = ':';
	put_u8_2digit(buf + 20, (unsigned)tm.tm_min);
	buf[22] = ':';
	put_u8_2digit(buf + 23, (unsigned)tm.tm_sec);
	buf[25] = ' ';
	buf[26] = 'G';
	buf[27] = 'M';
	buf[28] = 'T';
	buf[29] = '\0';
	ZEND_STATIC_ASSERT(HTTP_DATE_LEN == 29, "date length must match hand-format");
}

/* POSIX leaves it implementation-defined whether strptime rejects
 * out-of-range fields. Validate the canonical Gregorian ranges before
 * handing tm to timegm — otherwise day=99 / month=99 / sec=60 leak in
 * as a "valid" time_t. */
static bool tm_fields_in_range(const struct tm *tm)
{
	if (tm->tm_mon < 0 || tm->tm_mon > 11) {
		return false;
	}

	if (tm->tm_mday < 1 || tm->tm_mday > 31) {
		return false;
	}

	if (tm->tm_hour < 0 || tm->tm_hour > 23) {
		return false;
	}

	if (tm->tm_min < 0 || tm->tm_min > 59) {
		return false;
	}

	/* RFC 5322 / RFC 9110 do not require leap-second support; reject
	 * sec=60 outright rather than feeding undefined behavior to
	 * timegm. */
	if (tm->tm_sec < 0 || tm->tm_sec > 59) {
		return false;
	}

	return true;
}

/* RFC 9110 §5.6.7 accepts three formats; try in order of frequency. */
time_t http_date_parse_imf(const char *src, size_t src_len)
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
	if (strptime(buf, "%a, %d %b %Y %H:%M:%S GMT", &tm) != NULL && tm_fields_in_range(&tm)) {
		return timegm(&tm);
	}

	memset(&tm, 0, sizeof(tm));
	/* RFC 850:    "Sunday, 06-Nov-94 08:49:37 GMT" */
	if (strptime(buf, "%A, %d-%b-%y %H:%M:%S GMT", &tm) != NULL && tm_fields_in_range(&tm)) {
		return timegm(&tm);
	}

	memset(&tm, 0, sizeof(tm));
	/* asctime:    "Sun Nov  6 08:49:37 1994" */
	if (strptime(buf, "%a %b %e %H:%M:%S %Y", &tm) != NULL && tm_fields_in_range(&tm)) {
		return timegm(&tm);
	}

	return (time_t)-1;
}
