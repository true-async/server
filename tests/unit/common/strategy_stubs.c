/*
 * Stubs for what http2_strategy.c reaches outside the strategy vtable
 *
 * The strategy target builds the vtable and its lifecycle, not the connection
 * layer, the static handlers, the send-file path, the gRPC call or the response
 * object. Each stub aborts rather than answers: the three cases here never
 * dispatch a request, and a stub that invented an answer would turn a test that
 * did into a green one proving nothing.
 */

#include "php.h"
#include "php_http_server.h"
#include "http2/http2_stream.h"
#include "static/static_handler.h"
#include "http_send_file.h"
#include "grpc_call.h"

#include <stdlib.h>

void h2_static_account_debit(http_connection_t *conn, size_t n)
{
	(void) conn; (void) n;
	abort();
}

int h2_stream_send_static_response(void *ctx, zend_object *response_obj, zend_async_io_t *file_io,
		uint64_t body_offset, uint64_t body_length, bool head_only,
		void (*on_done)(void *user, int status), void *user)
{
	(void) ctx; (void) response_obj; (void) file_io; (void) body_offset;
	(void) body_length; (void) head_only; (void) on_done; (void) user;
	abort();
}

void grpc_call_ensure_status(zend_object *response_obj, bool had_exception)
{
	(void) response_obj; (void) had_exception;
	abort();
}

void grpc_call_finish(zend_object *response_obj, const grpc_finish_ops_t *ops, void *ctx)
{
	(void) response_obj; (void) ops; (void) ctx;
	abort();
}

void grpc_call_init_response(zend_object *response_obj, int grpc_mode)
{
	(void) response_obj; (void) grpc_mode;
	abort();
}

void http_connection_destroy_if_idle_deferred(http_connection_t *conn)
{
	(void) conn;
	abort();
}

bool http_connection_send_batched_writev(http_connection_t *conn, const zend_async_buf_t *iov,
		const unsigned niov, zend_async_io_write_free_cb_t free_cb, void *user_data)
{
	(void) conn; (void) iov; (void) niov; (void) free_cb; (void) user_data;
	abort();
}

void http_handler_log_bailout(const char *proto, const void *coroutine, const char *method, const char *uri)
{
	(void) proto; (void) coroutine; (void) method; (void) uri;
	abort();
}

zend_async_scope_t *http_request_scope_new(zend_async_scope_t *server_scope)
{
	(void) server_scope;
	abort();
}

bool http_response_has_send_file(zend_object *obj)
{
	(void) obj;
	abort();
}

http_send_file_request_t *http_response_take_send_file(zend_object *obj)
{
	(void) obj;
	abort();
}

bool http_response_commit_content_length(zend_object *obj)
{
	(void) obj;
	abort();
}

bool http_response_header_allowed_h2h3(const char *name, size_t len,
                                       bool keep_content_length)
{
	(void) name; (void) len; (void) keep_content_length;
	abort();
}

void http_response_set_default_json_flags(zend_object *obj, uint32_t flags)
{
	(void) obj; (void) flags;
	abort();
}

void http_response_set_head(zend_object *obj, bool is_head)
{
	(void) obj; (void) is_head;
	abort();
}

void http_response_static_set_body_str(zend_object *obj, zend_string *body)
{
	(void) obj; (void) body;
	abort();
}

void http_response_static_set_header(zend_object *obj, const char *name, size_t name_len,
		const char *value, size_t value_len)
{
	(void) obj; (void) name; (void) name_len; (void) value; (void) value_len;
	abort();
}

void http_response_static_set_status(zend_object *obj, int status_code)
{
	(void) obj; (void) status_code;
	abort();
}

bool http_send_file_dispatch(http_request_t *request, zend_object *response_obj,
		http_send_file_request_t *req, void (*on_done)(void *user, int status), void *user)
{
	(void) request; (void) response_obj; (void) req; (void) on_done; (void) user;
	abort();
}

HashTable *http_server_get_protocol_handlers(http_server_object *server)
{
	(void) server;
	abort();
}

size_t http_static_handler_count(const http_server_object *server)
{
	(void) server;
	abort();
}

http_static_result_t http_static_try_serve(http_server_object *server, struct http_request_t *request,
		zend_object *response_obj, http_server_counters_t *counters,
		const http_static_dispatch_cbs_t *cbs, void *user)
{
	(void) server; (void) request; (void) response_obj; (void) counters; (void) cbs; (void) user;
	abort();
}

int64_t http_response_get_declared_length(zend_object *obj)
{
	(void) obj;
	abort();
}

bool http_response_has_declared_length(zend_object *obj)
{
	(void) obj;
	abort();
}

bool http_response_keeps_declared_length(zend_object *obj)
{
	(void) obj;
	abort();
}
