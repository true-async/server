/* Unit tests for recognising a form in a Content-Type value: the media type
 * that makes a body a form, and the multipart boundary. Both read the byte
 * range they are given and nothing else. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#include "formats/form_content_type.h"

static http_form_kind_t kind_of(const char *value)
{
	return http_form_kind_of(value, strlen(value));
}

/* The boundary as a string, or "" when none is found. */
static const char *boundary_of(const char *value)
{
	static char found[HTTP_FORM_BOUNDARY_MAX_LEN + 1];
	const char *boundary = NULL;
	size_t boundary_len = 0;

	if (!http_form_find_boundary(value, strlen(value), &boundary, &boundary_len)) {
		return "";
	}

	memcpy(found, boundary, boundary_len);
	found[boundary_len] = '\0';
	return found;
}

static void test_empty_input_is_no_form(void **state)
{
	(void)state;

	assert_int_equal(http_form_kind_of(NULL, 0), HTTP_FORM_NONE);
	assert_int_equal(kind_of(""), HTTP_FORM_NONE);
	assert_int_equal(kind_of("   "), HTTP_FORM_NONE);
	assert_int_equal(kind_of(";boundary=x"), HTTP_FORM_NONE);
	assert_string_equal(boundary_of(""), "");
}

static void test_media_types(void **state)
{
	(void)state;

	assert_int_equal(kind_of("application/x-www-form-urlencoded"), HTTP_FORM_URLENCODED);
	assert_int_equal(kind_of("multipart/form-data; boundary=x"), HTTP_FORM_MULTIPART);
	assert_int_equal(kind_of("application/json"), HTTP_FORM_NONE);
	assert_int_equal(kind_of("text/plain"), HTTP_FORM_NONE);
	assert_int_equal(kind_of("multipart/mixed; boundary=x"), HTTP_FORM_NONE);
}

/* RFC 9110 §8.3.1: type and subtype are case-insensitive. */
static void test_media_type_ignores_case(void **state)
{
	(void)state;

	assert_int_equal(kind_of("Application/X-WWW-Form-Urlencoded"), HTTP_FORM_URLENCODED);
	assert_int_equal(kind_of("MULTIPART/FORM-DATA; boundary=x"), HTTP_FORM_MULTIPART);
}

static void test_media_type_is_a_whole_token(void **state)
{
	(void)state;

	assert_int_equal(kind_of("multipart/form-data-x; boundary=x"), HTTP_FORM_NONE);
	assert_int_equal(kind_of("application/x-www-form-urlencoded-not"), HTTP_FORM_NONE);
	assert_int_equal(kind_of("multipart/form-dat"), HTTP_FORM_NONE);
	assert_int_equal(kind_of("xmultipart/form-data"), HTTP_FORM_NONE);
}

static void test_whitespace_and_parameters_around_the_type(void **state)
{
	(void)state;

	assert_int_equal(kind_of("  application/x-www-form-urlencoded  "), HTTP_FORM_URLENCODED);
	assert_int_equal(kind_of("\tapplication/x-www-form-urlencoded\t; charset=UTF-8"),
					 HTTP_FORM_URLENCODED);
	assert_int_equal(kind_of("multipart/form-data;boundary=x"), HTTP_FORM_MULTIPART);
}

/* The range ends where the caller says, not at a NUL. */
static void test_only_the_given_range_is_read(void **state)
{
	(void)state;
	const char *const value = "multipart/form-data; boundary=abcdef";
	const char *boundary = NULL;
	size_t boundary_len = 0;

	assert_int_equal(http_form_kind_of(value, strlen("multipart/form")), HTTP_FORM_NONE);
	assert_true(http_form_find_boundary(value, strlen(value) - 3, &boundary, &boundary_len));
	assert_int_equal(boundary_len, 3);
	assert_memory_equal(boundary, "abc", 3);
	assert_false(
		http_form_find_boundary(value, strlen("multipart/form-data"), &boundary, &boundary_len));
}

static void test_boundary_token_and_quoted(void **state)
{
	(void)state;

	assert_string_equal(boundary_of("multipart/form-data; boundary=----WebKit123"),
						"----WebKit123");
	assert_string_equal(boundary_of("multipart/form-data; boundary=\"a b:c\""), "a b:c");
	assert_string_equal(boundary_of("multipart/form-data; charset=utf-8; boundary=x; y=z"), "x");
}

static void test_boundary_parameter_name_ignores_case(void **state)
{
	(void)state;

	assert_string_equal(boundary_of("multipart/form-data; Boundary=x"), "x");
	assert_string_equal(boundary_of("multipart/form-data; BOUNDARY=\"y\""), "y");
}

static void test_first_boundary_wins(void **state)
{
	(void)state;

	assert_string_equal(boundary_of("multipart/form-data; boundary=first; boundary=second"),
						"first");
}

static void test_boundary_rejections(void **state)
{
	(void)state;

	assert_string_equal(boundary_of("multipart/form-data"), "");
	assert_string_equal(boundary_of("multipart/form-data;"), "");
	assert_string_equal(boundary_of("multipart/form-data; boundary="), "");
	assert_string_equal(boundary_of("multipart/form-data; boundary=\"\""), "");
	assert_string_equal(boundary_of("multipart/form-data; boundary"), "");
	/* A parameter whose name merely ends in "boundary" is another parameter. */
	assert_string_equal(boundary_of("multipart/form-data; xboundary=x"), "");
}

/* RFC 2046 §5.1.1: 1 to 70 characters. */
static void test_boundary_length_limits(void **state)
{
	(void)state;
	char value[128];
	char expected[HTTP_FORM_BOUNDARY_MAX_LEN + 2];

	memset(expected, 'b', sizeof(expected));
	expected[HTTP_FORM_BOUNDARY_MAX_LEN] = '\0';
	snprintf(value, sizeof(value), "multipart/form-data; boundary=%s", expected);
	assert_string_equal(boundary_of(value), expected);

	snprintf(value, sizeof(value), "multipart/form-data; boundary=\"%s\"", expected);
	assert_string_equal(boundary_of(value), expected);

	expected[HTTP_FORM_BOUNDARY_MAX_LEN] = 'b';
	expected[HTTP_FORM_BOUNDARY_MAX_LEN + 1] = '\0';
	snprintf(value, sizeof(value), "multipart/form-data; boundary=%s", expected);
	assert_string_equal(boundary_of(value), "");

	assert_string_equal(boundary_of("multipart/form-data; boundary=z"), "z");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_empty_input_is_no_form),
		cmocka_unit_test(test_media_types),
		cmocka_unit_test(test_media_type_ignores_case),
		cmocka_unit_test(test_media_type_is_a_whole_token),
		cmocka_unit_test(test_whitespace_and_parameters_around_the_type),
		cmocka_unit_test(test_only_the_given_range_is_read),
		cmocka_unit_test(test_boundary_token_and_quoted),
		cmocka_unit_test(test_boundary_parameter_name_ignores_case),
		cmocka_unit_test(test_first_boundary_wins),
		cmocka_unit_test(test_boundary_rejections),
		cmocka_unit_test(test_boundary_length_limits),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
