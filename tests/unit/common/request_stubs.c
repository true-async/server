/*
 * Stubs for the request-finalize tail of http_request.c
 *
 * The parser targets drive bytes through the parser and stop there: nothing in
 * them finalizes a request, so the access-log tail is never reached. These
 * exist to satisfy the linker without dragging in the log rings and the
 * response object. Reaching one means the test is exercising a path this
 * target does not build, and an answer invented here would make it pass
 * anyway — so they abort instead.
 */

#include "php.h"

#include <stdlib.h>

struct http_log_state_t;
struct http_log_access_record_t;

int http_response_get_status(zend_object *obj)
{
	(void) obj;
	abort();
}

bool http_response_is_aborted(zend_object *obj)
{
	(void) obj;
	abort();
}

uint64_t http_response_get_sent_body_size(zend_object *obj)
{
	(void) obj;
	abort();
}

size_t http_response_get_body_len(zend_object *obj)
{
	(void) obj;
	abort();
}

void http_log_emit_access(struct http_log_state_t *state, const struct http_log_access_record_t *record)
{
	(void) state;
	(void) record;
	abort();
}

/* Throwing through a NULL class entry raises a plain Error, which is what a
 * test asserting the throw itself would see. No target asserts the class. */
zend_class_entry *http_server_runtime_exception_ce = NULL;
