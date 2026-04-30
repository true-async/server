/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_KNOWN_STRINGS_H
#define HTTP_KNOWN_STRINGS_H

#include <php.h>
#include <stddef.h>

/* Interned zend_string pool for values that repeat on every request —
 * primarily the HTTP method token. A miss returns NULL; the caller then
 * falls back to zend_string_init() so unknown/extension methods still
 * work. Initialised once from PHP_MINIT, never freed (interned strings
 * live for the lifetime of the process). */

void http_known_strings_minit(void);

/* Look up a method token by (bytes, length). Returns a borrowed pointer
 * to an interned zend_string on hit, or NULL on miss. The returned
 * string is safe to assign directly to req->method — zend_string_release
 * is a no-op on interned strings. */
zend_string *http_known_method_lookup(const char *name, size_t len);

/* Look up a lowercase HTTP header name. Same semantics as
 * http_known_method_lookup: returns a borrowed interned zend_string on
 * hit (safe to use as a HashTable key without zend_string_release), NULL
 * on miss. The caller must pass already-lowercased bytes (HTTP/2 names
 * arrive lowercase per RFC 9113 §8.2.1; HTTP/1 lowercases in
 * save_current_header before calling).
 *
 * Hits avoid zend_string_init + the per-request release cycle and let
 * the HashTable reuse the precomputed hash on the interned string. */
zend_string *http_known_header_lookup(const char *name, size_t len);

#endif /* HTTP_KNOWN_STRINGS_H */
