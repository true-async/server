/*
 * Stubs for what http2_session.c reaches outside the session itself
 *
 * The session target builds the session and the stream, not the strategy that
 * owns the connection, the static-file accounting or the response object. Each
 * stub below aborts rather than answers: a session test that reaches one is
 * asserting something this target cannot produce, and an invented answer would
 * let it pass while proving nothing.
 */

#include "php.h"

#include <stdlib.h>

#include "php_http_server.h"
#include "http2/http2_session.h"
#include "http2/http2_stream.h"

void http2_session_emit(http2_session_t *session)
{
	(void) session;
	abort();
}

void h2_static_account_debit(http_connection_t *conn, size_t n)
{
	(void) conn;
	(void) n;
	abort();
}

HashTable *http_response_get_trailers(zend_object *obj)
{
	(void) obj;
	abort();
}
