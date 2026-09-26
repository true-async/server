/* Unit tests for keying query and form variables by PHP's name rules. The
 * phpt http1parser/014 compares whole arrays with parse_str(); these hold the
 * edges parse_str() cannot be asked about: the limits, a name cut at NUL, and
 * a value that must keep every byte. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include "php.h"
#include "http_form_vars.h"
#include "common/php_sapi_test.h"

#define DEPTH 64
#define VARS 1000

static HashTable *decode_with(const char *data, const char *separators, const zend_long max_vars,
							  const zend_long max_depth, http_form_vars_result_t *result)
{
	HashTable *const target = zend_new_array(0);

	*result = http_form_vars_decode(target, data, strlen(data), separators, max_vars, max_depth);
	return target;
}

static HashTable *decode(const char *data)
{
	http_form_vars_result_t result;
	HashTable *const target = decode_with(data, "&", VARS, DEPTH, &result);

	assert_int_equal(result, HTTP_FORM_VARS_OK);
	return target;
}

static zval *at(HashTable *table, const char *key)
{
	zval *const value = zend_symtable_str_find(table, key, strlen(key));

	assert_non_null(value);
	return value;
}

static void assert_string_at(HashTable *table, const char *key, const char *expected)
{
	const zval *const value = at(table, key);

	assert_int_equal(Z_TYPE_P(value), IS_STRING);
	assert_int_equal(Z_STRLEN_P(value), strlen(expected));
	assert_memory_equal(Z_STRVAL_P(value), expected, strlen(expected));
}

static void test_empty_input(void **state)
{
	(void)state;
	HashTable *const table = decode("");

	assert_int_equal(zend_hash_num_elements(table), 0);
	zend_array_destroy(table);
}

static void test_single_pair_and_missing_value(void **state)
{
	(void)state;
	HashTable *const table = decode("a=1&flag&empty=");

	assert_string_at(table, "a", "1");
	assert_string_at(table, "flag", "");
	assert_string_at(table, "empty", "");
	zend_array_destroy(table);
}

static void test_percent_and_plus_decoding(void **state)
{
	(void)state;
	HashTable *const table = decode("v=a+b%2Bc%41&bad=%zz%4");

	assert_string_at(table, "v", "a b+cA");
	/* A malformed escape stays as it was written. */
	assert_string_at(table, "bad", "%zz%4");
	zend_array_destroy(table);
}

/* A value is binary: %00 decodes to a NUL and the bytes after it stay. */
static void test_value_keeps_nul_bytes(void **state)
{
	(void)state;
	HashTable *const table = decode("v=x%00y");
	const zval *const value = at(table, "v");

	assert_int_equal(Z_STRLEN_P(value), 3);
	assert_memory_equal(Z_STRVAL_P(value), "x\0y", 3);
	zend_array_destroy(table);
}

/* A name ends at NUL, as the C string PHP walks does. */
static void test_name_ends_at_nul(void **state)
{
	(void)state;
	HashTable *const table = decode("ab%00cd=1");

	assert_string_at(table, "ab", "1");
	assert_int_equal(zend_hash_num_elements(table), 1);
	zend_array_destroy(table);
}

static void test_base_name_mangling(void **state)
{
	(void)state;
	HashTable *const table = decode("a.b=1&c+d=2&++lead=3&x[a.b]=4");

	assert_string_at(table, "a_b", "1");
	assert_string_at(table, "c_d", "2");
	assert_string_at(table, "lead", "3");
	/* Only the base name is mangled; an index keeps its dots. */
	assert_string_at(Z_ARRVAL_P(at(table, "x")), "a.b", "4");
	zend_array_destroy(table);
}

static void test_empty_names_are_dropped(void **state)
{
	(void)state;
	HashTable *const table = decode("=1&[x]=2&+=3&ok=4");

	assert_int_equal(zend_hash_num_elements(table), 1);
	assert_string_at(table, "ok", "4");
	zend_array_destroy(table);
}

static void test_append_nest_and_numeric_keys(void **state)
{
	(void)state;
	HashTable *const table = decode("l[]=a&l[ ]=b&m[k][0]=c&7=d&m[k][x]=e");
	HashTable *const list = Z_ARRVAL_P(at(table, "l"));
	HashTable *const inner = Z_ARRVAL_P(at(Z_ARRVAL_P(at(table, "m")), "k"));

	assert_string_equal(Z_STRVAL_P(zend_hash_index_find(list, 0)), "a");
	assert_string_equal(Z_STRVAL_P(zend_hash_index_find(list, 1)), "b");
	assert_string_equal(Z_STRVAL_P(zend_hash_index_find(inner, 0)), "c");
	assert_string_at(inner, "x", "e");
	/* "7" is stored under the integer key 7. */
	assert_non_null(zend_hash_index_find(table, 7));
	zend_array_destroy(table);
}

static void test_unclosed_brackets(void **state)
{
	(void)state;
	HashTable *const table = decode("a[b=1&c[d][e=2&f[g]h=3");

	assert_string_at(table, "a_b", "1");
	assert_string_at(Z_ARRVAL_P(at(table, "c")), "d", "2");
	assert_string_at(Z_ARRVAL_P(at(table, "f")), "g", "3");
	zend_array_destroy(table);
}

static void test_separators(void **state)
{
	(void)state;
	http_form_vars_result_t result;
	HashTable *const semicolon = decode_with("a=1;b=2&c=3", "&;", VARS, DEPTH, &result);

	assert_int_equal(zend_hash_num_elements(semicolon), 3);
	zend_array_destroy(semicolon);

	/* A form body splits on & alone. */
	HashTable *const ampersand = decode("a=1;b=2&c=3");

	assert_string_at(ampersand, "a", "1;b=2");
	zend_array_destroy(ampersand);
}

static void test_depth_limit(void **state)
{
	(void)state;
	http_form_vars_result_t result;

	HashTable *const at_limit = decode_with("a[1][2][3]=v", "&", VARS, 3, &result);
	assert_int_equal(result, HTTP_FORM_VARS_OK);
	zend_array_destroy(at_limit);

	/* One level past: dropped with everything already under its base name. */
	HashTable *const past = decode_with("a[x]=1&a[1][2][3][4]=v", "&", VARS, 3, &result);
	assert_int_equal(result, HTTP_FORM_VARS_TOO_DEEP);
	assert_null(zend_hash_str_find(past, "a", 1));
	zend_array_destroy(past);
}

static void test_count_limit(void **state)
{
	(void)state;
	http_form_vars_result_t result;

	HashTable *const at_limit = decode_with("a=1&&b=2&c=3", "&", 3, DEPTH, &result);
	assert_int_equal(result, HTTP_FORM_VARS_OK);
	assert_int_equal(zend_hash_num_elements(at_limit), 3);
	zend_array_destroy(at_limit);

	HashTable *const past = decode_with("a=1&b=2&c=3&d=4", "&", 3, DEPTH, &result);
	assert_int_equal(result, HTTP_FORM_VARS_TOO_MANY);
	assert_int_equal(zend_hash_num_elements(past), 3);
	zend_array_destroy(past);
}

/* register() takes the value in every case, stored or dropped. */
static void test_register_takes_the_value(void **state)
{
	(void)state;
	HashTable *const table = zend_new_array(0);
	zval value;

	ZVAL_STR(&value, zend_string_init("dropped", 7, 0));
	assert_int_equal(http_form_vars_register(table, "   ", 3, &value, DEPTH), HTTP_FORM_VARS_OK);

	ZVAL_STR(&value, zend_string_init("kept", 4, 0));
	assert_int_equal(http_form_vars_register(table, "k[a]", 4, &value, DEPTH), HTTP_FORM_VARS_OK);
	assert_string_at(Z_ARRVAL_P(at(table, "k")), "a", "kept");
	zend_array_destroy(table);
}

static int suite_setup(void **state)
{
	(void)state;
	return php_test_runtime_init();
}

static int suite_teardown(void **state)
{
	(void)state;
	php_test_runtime_shutdown();
	return 0;
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_empty_input),
		cmocka_unit_test(test_single_pair_and_missing_value),
		cmocka_unit_test(test_percent_and_plus_decoding),
		cmocka_unit_test(test_value_keeps_nul_bytes),
		cmocka_unit_test(test_name_ends_at_nul),
		cmocka_unit_test(test_base_name_mangling),
		cmocka_unit_test(test_empty_names_are_dropped),
		cmocka_unit_test(test_append_nest_and_numeric_keys),
		cmocka_unit_test(test_unclosed_brackets),
		cmocka_unit_test(test_separators),
		cmocka_unit_test(test_depth_limit),
		cmocka_unit_test(test_count_limit),
		cmocka_unit_test(test_register_takes_the_value),
	};

	return cmocka_run_group_tests(tests, suite_setup, suite_teardown);
}
