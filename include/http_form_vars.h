/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Query and form variables keyed the way PHP keys $_GET and $_POST.
 *
 * A name's base has its spaces and dots turned into `_`; `name[]` appends,
 * `name[key]` and `name[a][b]` nest, and a numeric key becomes an integer key.
 * PHP's own php_register_variable_ex is not used: it stores each key through
 * zend_string_init_interned, which during a request adds the key to
 * CG(interned_strings) until PHP request shutdown, and this server's PHP
 * request lasts until the process exits, so every distinct name a client sent
 * would stay in memory. Keys here are ordinary strings owned by the array. */

#ifndef TRUE_ASYNC_HTTP_FORM_VARS_H
#define TRUE_ASYNC_HTTP_FORM_VARS_H

#include "php.h"

typedef enum
{
	HTTP_FORM_VARS_OK = 0,
	/* More name segments than the depth limit. The variable is dropped, and
	 * so is everything already stored under its base name, as PHP does. */
	HTTP_FORM_VARS_TOO_DEEP,
	/* More variables than the count limit; the rest were not read. */
	HTTP_FORM_VARS_TOO_MANY,
} http_form_vars_result_t;

/* Stores `value` in `target` under the PHP variable `name` (`name_len` bytes,
 * not NUL-terminated; a NUL ends the name as it ends a C string). Takes the
 * value in every case: stored, or released when the name is empty after its
 * leading spaces, nests deeper than `max_depth` bracket levels, or spells a
 * `__Host-`/`__Secure-` key the name as sent does not begin with. */
http_form_vars_result_t http_form_vars_register(HashTable *target, const char *name,
												size_t name_len, zval *value, zend_long max_depth);

/* Runs sapi_module.input_filter over a decoded value, as PHP does before it
 * stores a GET, POST or multipart variable. False when the filter drops the
 * variable. `*value` is an emalloc'd buffer on entry: the filter may efree it
 * and put a new one in its place, and the caller frees whichever `*value`
 * holds afterwards. `name` is NUL-terminated. */
bool http_form_vars_filter(const char *name, char **value, size_t value_len, size_t *filtered_len);

/* Decodes `name=value` pairs, percent-encoded with `+` for space, into `target`,
 * passing each value through http_form_vars_filter. A pair
 * ends at any byte of `separators` (NUL-terminated; "&" for a form body,
 * arg_separator.input for a query string); an empty pair is skipped and a
 * pair without `=` has the empty value. A name deeper than `max_depth` is
 * dropped and the rest is still read (the result says TOO_DEEP); past
 * `max_vars` pairs decoding stops (TOO_MANY), keeping what was stored. */
http_form_vars_result_t http_form_vars_decode(HashTable *target, const char *data, size_t data_len,
											  const char *separators, zend_long max_vars,
											  zend_long max_depth);

#endif
