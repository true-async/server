/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* The name rules follow php_register_variable_ex in main/php_variables.c,
 * case for case; the reason it is not called is in http_form_vars.h. Percent
 * decoding is php_url_decode: here it decodes values, not paths, so the
 * CODING_STANDARDS 13c.3 objection to it (it lets %00 and \ through) does not
 * apply, and $_POST decodes with it too. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "ext/standard/url.h"
#include "SAPI.h"
#include "main/php_variables.h"
#include "http_form_vars.h"

#include <string.h>

/* isspace() in the C locale, which is what PHP's rule reads. */
static inline bool is_c_space(const char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static inline bool is_separator(const char c, const char *separators)
{
	return c != '\0' && strchr(separators, c) != NULL;
}

/* PHP's name mangling for a base name: a space or a dot is not allowed there. */
static void mangle_base(char *from, const char *to, const bool bracket_too)
{
	for (char *p = from; p < to; p++) {
		if (*p == ' ' || *p == '.' || (bracket_too && *p == '[')) {
			*p = '_';
		}
	}
}

/* php_is_forbidden_variable_name: a key spelling a cookie prefix is refused
 * unless the name as sent begins with it, so "a[__Host-x]" cannot plant a key
 * that later code reads as a prefixed cookie. */
static bool is_forbidden_key(const char *key, const size_t key_len, const char *name,
							 const size_t name_len)
{
	static const struct
	{
		const char *text;
		size_t len;
	} prefixes[] = {
		{"__Host-", sizeof("__Host-") - 1},
		{"__Secure-", sizeof("__Secure-") - 1},
	};

	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		if (key_len >= prefixes[i].len && memcmp(key, prefixes[i].text, prefixes[i].len) == 0 &&
			(name_len < prefixes[i].len || memcmp(name, prefixes[i].text, prefixes[i].len) != 0)) {
			return true;
		}
	}

	return false;
}

/* The array one level down: appended when `append`, else the entry at `key`,
 * made into a fresh array when it holds anything else. NULL when the table
 * has no next index left. */
static HashTable *descend(HashTable *table, const char *key, const size_t key_len,
						  const bool append)
{
	zval fresh;

	if (append) {
		array_init(&fresh);
		zval *const slot = zend_hash_next_index_insert(table, &fresh);

		if (slot == NULL) {
			zend_array_destroy(Z_ARR(fresh));
			return NULL;
		}

		return Z_ARRVAL_P(slot);
	}

	zval *slot = zend_symtable_str_find(table, key, key_len);

	if (slot == NULL) {
		array_init(&fresh);
		slot = zend_symtable_str_update(table, key, key_len, &fresh);
	} else if (Z_TYPE_P(slot) != IS_ARRAY) {
		zval_ptr_dtor_nogc(slot);
		array_init(slot);
	} else {
		SEPARATE_ARRAY(slot);
	}

	return Z_ARRVAL_P(slot);
}

static void store(HashTable *table, const char *key, const size_t key_len, const bool append,
				  zval *value)
{
	if (!append) {
		zend_symtable_str_update(table, key, key_len, value);
		return;
	}

	if (zend_hash_next_index_insert(table, value) == NULL) {
		zval_ptr_dtor_nogc(value);
	}
}

http_form_vars_result_t http_form_vars_register(HashTable *target, const char *name,
												size_t name_len, zval *value,
												const zend_long max_depth)
{
	while (name_len > 0 && *name == ' ') {
		name++;
		name_len--;
	}

	const char *const nul = memchr(name, '\0', name_len);

	if (nul != NULL) {
		name_len = (size_t)(nul - name);
	}

	ALLOCA_FLAG(use_heap)
	char *const buffer = do_alloca(name_len + 1, use_heap);
	const char *const end = buffer + name_len;

	memcpy(buffer, name, name_len);
	buffer[name_len] = '\0';

	char *cursor = memchr(buffer, '[', name_len);

	if (cursor == NULL) {
		cursor = buffer + name_len;
	}

	mangle_base(buffer, cursor, false);

	const size_t base_len = (size_t)(cursor - buffer);

	if (base_len == 0) {
		zval_ptr_dtor_nogc(value);
		free_alloca(buffer, use_heap);
		return HTTP_FORM_VARS_OK;
	}

	HashTable *table = target;
	const char *key = buffer;
	size_t key_len = base_len;
	bool append = false;
	zend_long depth = 0;

	/* cursor is at a '[' on every pass */
	while (cursor < end) {
		if (++depth > max_depth) {
			zend_symtable_str_del(target, buffer, base_len);
			zval_ptr_dtor_nogc(value);
			free_alloca(buffer, use_heap);
			return HTTP_FORM_VARS_TOO_DEEP;
		}

		char *const index = cursor + 1;
		char *const close = memchr(index, ']', (size_t)(end - index));

		/* An unclosed bracket is no index. On the first level PHP joins it
		 * to the base name ("a[b" is "a_b"); deeper, the value lands under
		 * the last complete key and the rest is ignored. */
		if (close == NULL) {
			if (depth == 1) {
				mangle_base(cursor, end, true);
				key_len = name_len;
			}

			break;
		}

		if (!append && is_forbidden_key(key, key_len, name, name_len)) {
			zval_ptr_dtor_nogc(value);
			free_alloca(buffer, use_heap);
			return HTTP_FORM_VARS_OK;
		}

		table = descend(table, key, key_len, append);

		if (UNEXPECTED(table == NULL)) {
			zval_ptr_dtor_nogc(value);
			free_alloca(buffer, use_heap);
			return HTTP_FORM_VARS_OK;
		}

		/* "[]" and "[ ]" append; PHP skips one space before looking for ']' */
		append = close == index || (close == index + 1 && is_c_space(*index));
		key = index;
		key_len = (size_t)(close - index);
		cursor = close + 1;

		if (cursor >= end || *cursor != '[') {
			break;
		}
	}

	if (!append && is_forbidden_key(key, key_len, name, name_len)) {
		zval_ptr_dtor_nogc(value);
	} else {
		store(table, key, key_len, append, value);
	}

	free_alloca(buffer, use_heap);
	return HTTP_FORM_VARS_OK;
}

bool http_form_vars_filter(const char *name, char **value, const size_t value_len,
						   size_t *filtered_len)
{
	*filtered_len = value_len;

	if (sapi_module.input_filter == NULL) {
		return true;
	}

	/* PARSE_STRING, the argument parse_str() passes, whatever the source: for
	 * PARSE_POST and PARSE_GET ext/filter also stores the variable in $_POST or
	 * $_GET and in its INPUT_* copies, which belong to the whole worker here,
	 * not to one request, and stores it through the interning registration. */
	return sapi_module.input_filter(PARSE_STRING, name, value, value_len, filtered_len) != 0;
}

static http_form_vars_result_t decode_pair(HashTable *target, const char *pair,
										   const size_t pair_len, const zend_long max_depth)
{
	const char *const equals = memchr(pair, '=', pair_len);
	const size_t name_len = equals != NULL ? (size_t)(equals - pair) : pair_len;
	const char *const raw_value = equals != NULL ? equals + 1 : pair + pair_len;
	const size_t raw_value_len = (size_t)(pair + pair_len - raw_value);

	char *const name = estrndup(pair, name_len);
	const size_t decoded_name_len = php_url_decode(name, name_len);
	char *value = estrndup(raw_value, raw_value_len);
	const size_t value_len = php_url_decode(value, raw_value_len);
	size_t filtered_len;
	http_form_vars_result_t result = HTTP_FORM_VARS_OK;

	if (http_form_vars_filter(name, &value, value_len, &filtered_len)) {
		zval stored;

		ZVAL_STRINGL_FAST(&stored, value, filtered_len);
		result = http_form_vars_register(target, name, decoded_name_len, &stored, max_depth);
	}

	efree(value);
	efree(name);
	return result;
}

http_form_vars_result_t http_form_vars_decode(HashTable *target, const char *data,
											  const size_t data_len, const char *separators,
											  const zend_long max_vars, const zend_long max_depth)
{
	const char *const end = data + data_len;
	const char *pair = data;
	zend_long count = 0;
	http_form_vars_result_t result = HTTP_FORM_VARS_OK;

	while (pair < end) {
		const char *pair_end = pair;

		while (pair_end < end && !is_separator(*pair_end, separators)) {
			pair_end++;
		}

		if (pair_end > pair) {
			if (++count > max_vars) {
				return HTTP_FORM_VARS_TOO_MANY;
			}

			/* A name too deep is dropped alone, as PHP drops it; the pairs
			 * after it are still read. */
			if (decode_pair(target, pair, (size_t)(pair_end - pair), max_depth) ==
				HTTP_FORM_VARS_TOO_DEEP) {
				result = HTTP_FORM_VARS_TOO_DEEP;
			}
		}

		pair = pair_end + 1;
	}

	return result;
}
