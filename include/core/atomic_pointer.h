/* Copyright (c) TrueAsync. Licensed under the Apache License, Version 2.0. */
#ifndef HTTP_ATOMIC_POINTER_H
#define HTTP_ATOMIC_POINTER_H
#include "Zend/zend_atomic.h"
#include <stdint.h>
/* Zend exposes pointer load/store, but no pointer CAS. Its portable 64-bit
 * atomics cover uintptr_t on every supported target (including Win64). */
typedef zend_atomic_int64 http_atomic_pointer_t;
static inline void *http_atomic_pointer_load(http_atomic_pointer_t *p)
{
	return (void *)(uintptr_t)zend_atomic_int64_load_ex(p);
}
static inline void *http_atomic_pointer_exchange(http_atomic_pointer_t *p, void *v)
{
	return (void *)(uintptr_t)zend_atomic_int64_exchange_ex(p, (int64_t)(uintptr_t)v);
}
static inline bool http_atomic_pointer_compare_exchange(http_atomic_pointer_t *p, void **expected,
														void *v)
{
	int64_t old = (int64_t)(uintptr_t)*expected;
	bool ok = zend_atomic_int64_compare_exchange_ex(p, &old, (int64_t)(uintptr_t)v);
	if (!ok) {
		*expected = (void *)(uintptr_t)old;
	}
	return ok;
}
#endif
