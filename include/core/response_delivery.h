/* Copyright (c) TrueAsync. Licensed under the Apache License, Version 2.0. */
#ifndef RESPONSE_DELIVERY_H
#define RESPONSE_DELIVERY_H

#include "php.h"
#include "Zend/zend_async_API.h"
#include "core/reactor_pool.h"

typedef struct response_delivery_owner_s response_delivery_owner_t;
typedef struct response_delivery_s response_delivery_t;
struct http_request_t;
struct _http_server_object;
struct response_wire_s;

/* One endpoint per worker generation, registered before publishing its inbox.
 * close runs after the inbox fence and the handler-scope drain, before logger
 * shutdown. It cancels unfinished transports and waits for their publications. */
response_delivery_owner_t *response_delivery_owner_create(void *server, reactor_pool_t *pool);
void response_delivery_owner_close(response_delivery_owner_t *owner);
void response_delivery_owner_free(response_delivery_owner_t *owner);

/* Reactor creates the record before dispatch. The worker owns the request;
 * neither terminal reporting nor reset depends on the request's last ref. */
response_delivery_t *response_delivery_create(response_delivery_owner_t *owner, int reactor,
											  void *stream, reactor_exec_fn reset,
											  reactor_exec_fn release_request,
											  reactor_exec_fn apply);
void response_delivery_abandon(response_delivery_t *d);
void response_delivery_addref(response_delivery_t *d);
void response_delivery_release(response_delivery_t *d);
void response_delivery_worker_begin(response_delivery_t *d, const struct http_request_t *req);
void response_delivery_sender_done(response_delivery_t *d, const struct http_request_t *req,
								   zend_object *response, bool report);
void response_delivery_sender_abandoned(response_delivery_t *d);
void response_delivery_own_wire(response_delivery_t *d, struct response_wire_s *wire);
void response_delivery_discard_owned(response_delivery_t *d);
void response_delivery_note_drop(response_delivery_t *d);
bool response_delivery_post(response_delivery_t *d, struct response_wire_s *wire,
							uint32_t timeout_ms, bool nonblocking);
void response_delivery_cancel(response_delivery_t *d, const char *reason);
bool response_delivery_failed(const response_delivery_t *d);
bool response_delivery_sendable(response_delivery_t *d);
bool response_delivery_wait_writable(response_delivery_t *d, uint32_t timeout_ms);

/* Reactor-only. finish and the sender's snapshot publish independently;
 * completion is queued once only when both are ready. */
void response_delivery_started(response_delivery_t *d, int status);
void response_delivery_ack(response_delivery_t *d, uint64_t bytes);
void response_delivery_ack_transport(response_delivery_t *d, uint64_t bytes);
void response_delivery_finish(response_delivery_t *d, bool clean, const char *reason);
void response_delivery_stream_gone(response_delivery_t *d);
void response_delivery_maybe_release_request(response_delivery_t *d);

/* Worker-only: post request-consumed control to the reactor. */
void response_delivery_request_consumed(response_delivery_t *d);

/* Any thread holding a record reference: track wire lifetime. */
void response_delivery_wire_acquire(response_delivery_t *d);
void response_delivery_wire_release(response_delivery_t *d);

#endif
