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

/* Validate canonical Gregorian ranges before converting to time_t. */
static bool tm_fields_in_range(const struct tm *tm)
{
	if (tm->tm_mon < 0 || tm->tm_mon > 11)   return false;
	if (tm->tm_mday < 1 || tm->tm_mday > 31) return false;
	if (tm->tm_hour < 0 || tm->tm_hour > 23) return false;
	if (tm->tm_min  < 0 || tm->tm_min  > 59) return false;
	/* RFC 5322/9110: no leap-second support; sec=60 is rejected. */
	if (tm->tm_sec  < 0 || tm->tm_sec  > 59) return false;
	return true;
}

/* Map a 3-char month abbreviation (case-sensitive) to 0-based index. */
static int http_date_month_from_abbr(const char mon[4])
{
	static const char months[12][4] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	for (int i = 0; i < 12; i++) {
		if (mon[0] == months[i][0] && mon[1] == months[i][1] && mon[2] == months[i][2])
			return i;
	}
	return -1;
}

/* MSVC has _mkgmtime which is identical in semantics to POSIX timegm. */
#ifdef PHP_WIN32
# define http_date_timegm _mkgmtime
#else
# define http_date_timegm timegm
#endif

/* RFC 9110 §5.6.7 accepts three formats; try in order of frequency.
 * Reimplemented with sscanf so it builds on MSVC without strptime. */
time_t http_date_parse_imf(const char *src, size_t src_len)
{
	char buf[64];

	if (src_len == 0 || src_len >= sizeof(buf)) {
		return (time_t)-1;
	}

	memcpy(buf, src, src_len);
	buf[src_len] = '\0';

	struct tm tm;
	int mday, year, hour, min, sec;
	char mon[4];

	/* IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT" — comma at position 3 */
	if (src_len >= 26 && buf[3] == ',') {
		memset(&tm, 0, sizeof(tm));
		memset(mon, 0, sizeof(mon));
		if (sscanf(buf + 5, "%d %3s %d %d:%d:%d",
		           &mday, mon, &year, &hour, &min, &sec) == 6) {
			int m = http_date_month_from_abbr(mon);
			if (m >= 0) {
				tm.tm_mday = mday; tm.tm_mon = m; tm.tm_year = year - 1900;
				tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = sec;
				if (tm_fields_in_range(&tm)) return http_date_timegm(&tm);
			}
		}
		return (time_t)-1;
	}

	/* RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT" — full day name, comma not at [3] */
	{
		const char *comma = memchr(buf, ',', src_len);
		if (comma != NULL) {
			memset(&tm, 0, sizeof(tm));
			memset(mon, 0, sizeof(mon));
			if (sscanf(comma + 2, "%d-%3s-%d %d:%d:%d",
			           &mday, mon, &year, &hour, &min, &sec) == 6) {
				int m = http_date_month_from_abbr(mon);
				if (m >= 0) {
					tm.tm_mday = mday; tm.tm_mon = m;
					tm.tm_year = year < 69 ? year + 100 : year;
					tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = sec;
					if (tm_fields_in_range(&tm)) return http_date_timegm(&tm);
				}
			}
			return (time_t)-1;
		}
	}

	/* asctime: "Sun Nov  6 08:49:37 1994" — no comma */
	memset(&tm, 0, sizeof(tm));
	memset(mon, 0, sizeof(mon));
	if (sscanf(buf, "%*s %3s %d %d:%d:%d %d",
	           mon, &mday, &hour, &min, &sec, &year) == 6) {
		int m = http_date_month_from_abbr(mon);
		if (m >= 0) {
			tm.tm_mday = mday; tm.tm_mon = m; tm.tm_year = year - 1900;
			tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = sec;
			if (tm_fields_in_range(&tm)) return http_date_timegm(&tm);
		}
	}

	return (time_t)-1;
}
