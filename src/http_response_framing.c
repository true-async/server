/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* What a response says about its own framing: the Content-Length rule every
 * transport asks — HTTP/1 formats the answer into its block, the others put it
 * in an nv slot — and the filter that carries the answer out over HTTP/2 and
 * HTTP/3. Both read their arguments and nothing else, which is what lets the
 * unit suite hold them to a table of cases instead of to a wire. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "php_http_server.h"
#include "http_response_internal.h"   /* the two status predicates the rule reads */

http_response_length_action_t
http_response_length_action_for(const http_response_length_inputs_t *in)
{
    const int status = in->status;

    if (UNEXPECTED(!response_status_carries_body(status))) {
        if (response_status_needs_zero_length(status)) {
            return HTTP_RESPONSE_LENGTH_ZERO;
        }

        /* RFC 9110 §8.6 forbids the field on a 1xx and a 204 and permits it on
         * a 304, where it describes the representation a 200 would carry. */
        return status < 200 || status == 204
            ? HTTP_RESPONSE_LENGTH_OMIT
            : HTTP_RESPONSE_LENGTH_KEEP;
    }

    /* A stream has no buffer to measure; its own declaration is the only count
     * it can state, and finish_stream audits the bytes against it. */
    if (in->streaming) {
        return in->declared_length >= 0
            ? HTTP_RESPONSE_LENGTH_KEEP
            : HTTP_RESPONSE_LENGTH_OMIT;
    }

    if (in->length_stated) {
        return HTTP_RESPONSE_LENGTH_KEEP;
    }

    if (!in->is_head) {
        return HTTP_RESPONSE_LENGTH_FROM_BODY;
    }

    /* A HEAD carries the length its GET would have (RFC 9110 §9.3.2): the
     * buffer holds that body. A handler that streamed instead had its bytes
     * dropped, so the empty buffer measures nothing and only its own
     * declaration can answer. */
    if (in->table_has_length) {
        return HTTP_RESPONSE_LENGTH_KEEP;
    }

    return in->head_streamed
        ? HTTP_RESPONSE_LENGTH_OMIT
        : HTTP_RESPONSE_LENGTH_FROM_BODY;
}

bool http_response_header_allowed_h2h3(const char *name, const size_t len,
                                       const bool keep_content_length)
{
    switch (len) {
    case 7:
        return zend_binary_strcasecmp(name, 7, "upgrade", 7) != 0;
    case 10:
        return zend_binary_strcasecmp(name, 10, "connection", 10) != 0 &&
               zend_binary_strcasecmp(name, 10, "keep-alive", 10) != 0;
    case 14:
        /* Implicit from DATA frames, and dropped for that reason — unless the
         * response declared a length, which is the peer's own means of telling
         * a body that stopped from one that finished. */
        return keep_content_length
            || zend_binary_strcasecmp(name, 14, "content-length", 14) != 0;
    case 17:
        return zend_binary_strcasecmp(name, 17, "transfer-encoding", 17) != 0;
    default:
        return true;
    }
}
