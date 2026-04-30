/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_SERVER_SMART_STR_SCALABLE_H
#define HTTP_SERVER_SMART_STR_SCALABLE_H

#include <zend_smart_str.h>
#include <zend_alloc.h>

/*
 * smart_str has a hidden cliff: its default growth policy is PAGE-
 * aligned exact-needed (str->a = len + 4095 & ~4095), not geometric.
 * For buffers that stay below Zend MM's 2 MiB huge-alloc threshold the
 * policy is fine — reallocations happen inside the managed heap and
 * cost microseconds. Past that threshold the backing store flips to
 * mmap-backed storage, and every further grow becomes an mremap() call
 * that is proportional to the current allocation size. On a 256 MiB
 * body fed in 16 KiB HTTP/2 DATA chunks this observed as 40 000+
 * mremaps consuming half the request wall time.
 *
 * Fix: below the threshold, stay with default behaviour (cheap, no
 * waste). Above it, switch to 2× doubling so mremap count goes from
 * O(size/PAGE) to O(log(size)). For a 256 MiB body that's 7 calls
 * instead of 40 000. No preallocation overhead for small buffers.
 *
 * Use when the final size is unknown (chunked H1 request body,
 * streaming response write). When the size IS known, a single
 * smart_str_alloc(buf, size, 0) up front is still the right call.
 */
static zend_always_inline void
http_smart_str_append_scalable(smart_str *const buf,
                               const char *const data,
                               const size_t len)
{
    const size_t cur   = buf->s != NULL ? ZSTR_LEN(buf->s) : 0;
    const size_t alloc = buf->s != NULL ? buf->a : 0;
    const size_t needed = cur + len;

    /* Branch prediction: the slow path fires O(log N) times out of N
     * appends, so predicting "no grow" is right ≥ 99 % of the time for
     * a large upload. Default growth path remains hot inside
     * smart_str_appendl. */
    if (UNEXPECTED(needed > alloc && needed > ZEND_MM_CHUNK_SIZE)) {
        /* First jump past the threshold: reserve 4 MiB directly (skip
         * the linear-growth zone entirely). After that, double. */
        size_t reserve = (alloc < ZEND_MM_CHUNK_SIZE)
                         ? (2u * ZEND_MM_CHUNK_SIZE)
                         : (alloc * 2u);
        while (reserve < needed) {
            reserve *= 2u;
        }
        /* smart_str_alloc grows str->a to at least cur + delta. Compute
         * delta so final capacity hits `reserve`. */
        if (reserve > cur) {
            (void)smart_str_alloc(buf, reserve - cur, 0);
        }
    }
    smart_str_appendl(buf, data, len);
}

#endif /* HTTP_SERVER_SMART_STR_SCALABLE_H */
