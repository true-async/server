/* Copyright (c) TrueAsync. Licensed under the Apache License, Version 2.0. */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include "core/response_delivery.h"
#include "core/response_wire.h"
#include "core/atomic_pointer.h"
#include "core/bailout_guard.h"
#include "core/worker_dispatch.h"
#include "core/async_plain_event.h"
#include "php_http_server.h"
#include "http1/http_parser.h"
#include "log/http_log.h"
#include "zend_exceptions.h"
#include "Zend/zend_hrtime.h"
#include "fiu-local.h"
#include <stdlib.h>
#include <string.h>

enum
{
	DELIVERY_PENDING,
	DELIVERY_CANCEL_REQUESTED,
	DELIVERY_OK,
	DELIVERY_ERROR
};
enum
{
	SNAPSHOT_READY = 1,
	TERMINAL_READY = 2
};

typedef struct delivery_waiter_s
{
	struct delivery_waiter_s *next;
	struct delivery_waiter_s *prev;
	zend_async_event_t *event;
	response_delivery_t *delivery;
	volatile bool detached;
} delivery_waiter_t;

typedef struct
{
	reactor_capacity_gate_t gate;
	response_delivery_owner_t *owner;
	zend_atomic_bool waiting;
	delivery_waiter_t *waiters; /* worker-only: reactor signals the gate */
	delivery_waiter_t *tail;
} delivery_gate_t;

struct response_delivery_owner_s
{
	void *server;
	reactor_pool_t *pool;
	zend_async_trigger_event_t *wake;
	http_atomic_pointer_t completed;
	zend_atomic_int outstanding;
	delivery_gate_t *gates;
	int reactors;
	response_delivery_t *active; /* worker-only */
	bool closing;
	unsigned waiters;
};

struct response_delivery_s
{
	zend_atomic_int refs;
	zend_atomic_int state;
	zend_atomic_int ready;
	zend_atomic_int wires;
	zend_atomic_int dropped_wires;
	zend_atomic_bool published;
	response_delivery_owner_t *owner;
	reactor_pool_t *pool; /* remains valid through producer quiescence */
	int reactor;
	void *stream; /* reactor-only */
	reactor_exec_fn reset;
	reactor_exec_fn apply;
	reactor_exec_fn release_request;
	reactor_control_t cancel_node;
	reactor_control_t consumed_node;
	response_delivery_t *completed_next;
	response_delivery_t *active_next;
	response_delivery_t **active_link;
	bool request_consumed; /* reactor-only */
	bool request_released;
	int status; /* reactor-only: accepted by HTTP/3 */
	int submitted_status;
	uint64_t acked_body;
	const char *reason;		   /* fixed string, no allocation on cancellation */
	const char *cancel_reason; /* published by CAS state */
	http_access_rec_t access;  /* worker writes before SNAPSHOT_READY */
	char *method, *path, *query, *address;
	uint8_t trace_id[16], span_id[8];
	bool report;
	bool worker_begun;
	response_wire_t *owned_wire; /* worker-only, including bailout recovery */
	bool owned_ready;
};

static int delivery_ready(response_delivery_t *d, int bit)
{
	int old = zend_atomic_int_load_ex(&d->ready);
	while (!zend_atomic_int_compare_exchange_ex(&d->ready, &old, old | bit)) {
	}
	return old;
}

static void delivery_publish(response_delivery_t *d, int bit)
{
	const int old = delivery_ready(d, bit);
	if ((old | bit) != (SNAPSHOT_READY | TERMINAL_READY) ||
		zend_atomic_int_load_ex(&d->wires) != 0 ||
		zend_atomic_bool_exchange_ex(&d->published, true)) {
		return;
	}
	response_delivery_owner_t *o = d->owner;
	void *head = http_atomic_pointer_load(&o->completed);
	do {
		d->completed_next = head;
	} while (!http_atomic_pointer_compare_exchange(&o->completed, &head, d));
	o->wake->trigger(o->wake);
}

void response_delivery_addref(response_delivery_t *d)
{
	if (d != NULL) {
		zend_atomic_int_inc(&d->refs);
	}
}

void response_delivery_release(response_delivery_t *d)
{
	if (d == NULL || zend_atomic_int_fetch_sub(&d->refs, 1) != 1) {
		return;
	}
	free(d->method);
	free(d->path);
	free(d->query);
	free(d->address);
	free(d);
}

static void owner_drain(response_delivery_owner_t *o)
{
	response_delivery_t *head = http_atomic_pointer_exchange(&o->completed, NULL);
	while (head != NULL) {
		response_delivery_t *d = head;
		head = d->completed_next;
		http_server_object *server = o->server;
		http_server_counters_t *c = http_server_counters(server);
		if (d->report) {
			c->worker_wire_dropped_total += (uint64_t)zend_atomic_int_load_ex(&d->dropped_wires);
			const bool failed = zend_atomic_int_load_ex(&d->state) == DELIVERY_ERROR;
			if (d->status != 0) {
				http_server_count_request(c, d->status);
				if (failed) {
					c->responses_aborted_total++;
				}
			} else {
				c->total_requests++;
				c->responses_undelivered_total++;
			}
			d->access.status = d->status;
			d->access.response_size = d->acked_body;
			d->access.error_type =
				failed ? (d->status != 0 ? "response_aborted" : "response_undelivered") : NULL;
			http_log_emit_access(http_server_get_log_state(server), &d->access);
		}
		*d->active_link = d->active_next;
		if (d->active_next != NULL) {
			d->active_next->active_link = d->active_link;
		}
		zend_atomic_int_dec(&o->outstanding);
		d->owner = NULL;			  /* later request reclamation does not retain a logger */
		response_delivery_release(d); /* worker's pending-completion ref */
	}
}

static void owner_signal(zend_async_event_t *event, zend_async_event_callback_t *callback,
						 void *result, zend_object *exception)
{
	(void)callback;
	(void)result;
	(void)exception;
	response_delivery_owner_t *o =
		*(response_delivery_owner_t **)((char *)event + event->extra_offset);
	for (int i = 0; i < o->reactors; i++) {
		for (delivery_waiter_t *w = o->gates[i].waiters; w != NULL; w = w->next) {
			if (w == o->gates[i].waiters || o->closing ||
				zend_atomic_int_load_ex(&w->delivery->state) != DELIVERY_PENDING) {
				async_plain_event_fire(w->event);
			}
		}
	}
	owner_drain(o);
}

static void capacity_wake(void *arg)
{
	delivery_gate_t *g = arg;
	if (zend_atomic_bool_load_ex(&g->waiting)) {
		g->owner->wake->trigger(g->owner->wake);
	}
}

static void gate_add(delivery_gate_t *g, delivery_waiter_t *w)
{
	w->prev = g->tail;
	w->next = NULL;
	if (g->tail != NULL) {
		g->tail->next = w;
	} else {
		g->waiters = w;
	}
	g->tail = w;
	g->owner->waiters++;
	zend_atomic_bool_store_ex(&g->waiting, true);
}

static void gate_remove(delivery_gate_t *g, delivery_waiter_t *w, int reactor)
{
	if (w->prev != NULL) {
		w->prev->next = w->next;
	} else {
		g->waiters = w->next;
	}
	if (w->next != NULL) {
		w->next->prev = w->prev;
	} else {
		g->tail = w->prev;
	}
	g->owner->waiters--;
	zend_atomic_bool_store_ex(&g->waiting, g->waiters != NULL);
	/* An awakened sender may cancel instead of taking the available slot. */
	if (g->waiters != NULL &&
		(g->owner->closing ||
		 zend_atomic_int_load_ex(&g->waiters->delivery->state) != DELIVERY_PENDING ||
		 reactor_pool_has_capacity(g->owner->pool, reactor))) {
		async_plain_event_fire(g->waiters->event);
	}
	if (g->owner->closing) {
		g->owner->wake->trigger(g->owner->wake);
	}
}

/* Prepare every fallible resource before publishing a stack waiter. A local
 * bailout firewall retracts subscriptions before the C frame disappears. */
static bool wait_prepare(delivery_waiter_t *w, zend_coroutine_t *co, uint64_t deadline)
{
	volatile bool ok = false;
	volatile bool bailout = false;
	zend_async_timer_event_t *volatile timer = NULL;
	volatile bool timer_subscribed = false;
	http_bailout_state_t state;
	http_bailout_state_save(&state);
	zend_try
	{
		w->event = async_plain_event_new();
		if (w->event != NULL && ZEND_ASYNC_WAKER_NEW(co) != NULL) {
			zend_async_resume_when(co, w->event, false, zend_async_waker_callback_resolve, NULL);
			if (deadline != 0) {
				uint64_t now = zend_hrtime();
				timer = ZEND_ASYNC_NEW_TIMER_EVENT(
					now < deadline ? (deadline - now + 999999) / 1000000 : 1, false);
				if (timer != NULL) {
					zend_async_resume_when(co, &timer->base, true,
										   zend_async_waker_callback_timeout, NULL);
					timer_subscribed = true;
					ok = true;
				}
			} else {
				ok = true;
			}
		}
	}
	zend_catch
	{
		http_bailout_state_restore(&state);
		bailout = true;
	}
	zend_end_try();
	if (!ok) {
		ZEND_ASYNC_WAKER_DESTROY(co);
		if (timer != NULL && !timer_subscribed) {
			timer->base.dispose(&timer->base);
		}
		if (w->event != NULL) {
			w->event->dispose(w->event);
		}
		w->event = NULL;
	}
	if (bailout) {
		zend_bailout();
	}
	return ok;
}

static bool wait_finish(delivery_gate_t *g, delivery_waiter_t *w, zend_coroutine_t *co, int reactor,
						bool suspend)
{
	volatile bool bailout = false;
	http_bailout_state_t state;
	http_bailout_state_save(&state);
	zend_try
	{
		if (suspend) {
			fiu_do_on("h3/delivery/wait_bailout", { zend_bailout(); });
			ZEND_ASYNC_SUSPEND();
		}
	}
	zend_catch
	{
		http_bailout_state_restore(&state);
		bailout = true;
	}
	zend_end_try();
	ZEND_ASYNC_WAKER_DESTROY(co);
	bool attached = !w->detached;
	if (attached) {
		gate_remove(g, w, reactor);
	}
	w->event->dispose(w->event);
	if (bailout) {
		zend_bailout();
	}
	return attached;
}

response_delivery_owner_t *response_delivery_owner_create(void *server, reactor_pool_t *pool)
{
	if (server == NULL || pool == NULL) {
		return NULL;
	}
	response_delivery_owner_t *o = calloc(1, sizeof(*o));
	if (o == NULL) {
		return NULL;
	}
	o->server = server;
	o->pool = pool;
	o->reactors = reactor_pool_count(pool);
	ZEND_ATOMIC_INT64_INIT(&o->completed, 0);
	ZEND_ATOMIC_INT_INIT(&o->outstanding, 0);
	o->gates = calloc((size_t)o->reactors, sizeof(*o->gates));
	o->wake = ZEND_ASYNC_NEW_TRIGGER_EVENT_EX(sizeof(response_delivery_owner_t *));
	if (o->gates == NULL || o->wake == NULL) {
		goto fail;
	}
	*(response_delivery_owner_t **)((char *)&o->wake->base + o->wake->base.extra_offset) = o;
	zend_async_event_callback_t *cb = ZEND_ASYNC_EVENT_CALLBACK(owner_signal);
	if (cb == NULL || !o->wake->base.add_callback(&o->wake->base, cb)) {
		if (cb != NULL) {
			ZEND_ASYNC_EVENT_CALLBACK_RELEASE(cb);
		}
		goto fail;
	}
	int registered = 0;
	for (int i = 0; i < o->reactors; i++) {
		delivery_gate_t *g = &o->gates[i];
		g->owner = o;
		g->gate.wake = capacity_wake;
		g->gate.arg = g;
		ZEND_ATOMIC_BOOL_INIT(&g->waiting, false);
		if (!reactor_pool_capacity_register(pool, i, &g->gate)) {
			for (int j = 0; j < registered; j++) {
				reactor_pool_capacity_unregister(pool, j, &o->gates[j].gate);
			}
			goto fail;
		}
		registered++;
	}
	return o;
fail:
	if (o->wake != NULL) {
		o->wake->base.dispose(&o->wake->base);
	}
	free(o->gates);
	free(o);
	return NULL;
}

static void cancel_apply(void *arg)
{
	response_delivery_t *d = arg;
	response_delivery_finish(d, false, d->cancel_reason);
	if (d->stream != NULL && d->reset != NULL) {
		d->reset(d->stream);
	}
	response_delivery_release(d); /* control command */
}

static void consumed_apply(void *arg)
{
	response_delivery_t *d = arg;
	d->request_consumed = true;
	response_delivery_maybe_release_request(d);
	response_delivery_release(d);
}

response_delivery_t *response_delivery_create(response_delivery_owner_t *o, int reactor,
											  void *stream, reactor_exec_fn reset,
											  reactor_exec_fn release_request,
											  reactor_exec_fn apply)
{
	response_delivery_t *d = calloc(1, sizeof(*d));
	if (d == NULL) {
		return NULL;
	}
	d->owner = o;
	d->pool = o->pool;
	d->reactor = reactor;
	d->stream = stream;
	d->reset = reset;
	d->release_request = release_request;
	d->apply = apply;
	d->cancel_node.fn = cancel_apply;
	d->cancel_node.arg = d;
	d->consumed_node.fn = consumed_apply;
	d->consumed_node.arg = d;
	ZEND_ATOMIC_INT_INIT(&d->refs, 2); /* reactor stream + worker completion */
	ZEND_ATOMIC_INT_INIT(&d->state, DELIVERY_PENDING);
	ZEND_ATOMIC_INT_INIT(&d->ready, 0);
	ZEND_ATOMIC_INT_INIT(&d->wires, 0);
	ZEND_ATOMIC_INT_INIT(&d->dropped_wires, 0);
	ZEND_ATOMIC_BOOL_INIT(&d->published, false);
	zend_atomic_int_inc(&o->outstanding);
	return d;
}

void response_delivery_abandon(response_delivery_t *d)
{
	zend_atomic_int_dec(&d->owner->outstanding);
	response_delivery_release(d);
	response_delivery_release(d);
}


static char *copy_string(const char *s)
{
	if (s == NULL) {
		return NULL;
	}
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	if (p != NULL) {
		memcpy(p, s, n);
	}
	return p;
}

static void update_string(char **copy, const char *value)
{
	if (*copy != NULL && value != NULL && strcmp(*copy, value) == 0) {
		return;
	}
	char *next = copy_string(value);
	free(*copy);
	*copy = next;
}

static void delivery_snapshot(response_delivery_t *d, const http_request_t *req,
							  zend_object *response, bool report)
{
	char ip[INET6_ADDRSTRLEN];
	http_request_fill_access_rec(req, response, &d->access, ip, sizeof(ip));
	/* Request identity is immutable. Reuse its early snapshot; only tracing
	 * and service stamps become available after the handler has run. */
	update_string(&d->method, d->access.method);
	update_string(&d->path, d->access.url_path);
	update_string(&d->query, d->access.url_query);
	update_string(&d->address, d->access.client_address);
	d->access.method = d->method;
	d->access.url_path = d->path;
	d->access.url_query = d->query;
	d->access.client_address = d->address;
	if (req != NULL && req->has_trace) {
		memcpy(d->trace_id, req->trace_id, sizeof(d->trace_id));
		memcpy(d->span_id, req->span_id, sizeof(d->span_id));
		d->access.trace_id = d->trace_id;
		d->access.span_id = d->span_id;
	}
	d->report = report;
}

void response_delivery_worker_begin(response_delivery_t *d, const http_request_t *req)
{
	if (d->worker_begun) {
		return;
	}
	d->worker_begun = true;
	d->active_next = d->owner->active;
	d->active_link = &d->owner->active;
	if (d->active_next != NULL) {
		d->active_next->active_link = &d->active_next;
	}
	d->owner->active = d;
	/* Teardown can finish without resuming a cancelled handler. Preserve
	 * immutable request identity while the fields still belong to the worker. */
	delivery_snapshot(d, req, NULL, true);
}

void response_delivery_sender_abandoned(response_delivery_t *d)
{
	response_delivery_cancel(d, "dispatch_failed");
	delivery_publish(d, SNAPSHOT_READY); /* early identity snapshot is intact */
}

void response_delivery_sender_done(response_delivery_t *d, const http_request_t *req,
								   zend_object *response, bool report)
{
	if ((zend_atomic_int_load_ex(&d->ready) & SNAPSHOT_READY) != 0) {
		return;
	}
	delivery_snapshot(d, req, response, report);
	delivery_publish(d, SNAPSHOT_READY);
}

bool response_delivery_failed(const response_delivery_t *d)
{
	int state = zend_atomic_int_load_ex(&((response_delivery_t *)d)->state);
	return state == DELIVERY_CANCEL_REQUESTED || state == DELIVERY_ERROR;
}

void response_delivery_cancel(response_delivery_t *d, const char *reason)
{
	int expected = DELIVERY_PENDING;
	if (zend_atomic_int_load_ex(&d->state) != DELIVERY_PENDING) {
		return;
	}
	/* Only the worker calls cancel: publish the bounded reason before state. */
	d->cancel_reason = reason;
	if (!zend_atomic_int_compare_exchange_ex(&d->state, &expected, DELIVERY_CANCEL_REQUESTED)) {
		return;
	}
	response_delivery_addref(d);
	if (!reactor_pool_post_control(d->pool, d->reactor, &d->cancel_node)) {
		/* A stopped reactor has no writer left. It cannot deliver a response. */
		response_delivery_finish(d, false, reason);
		response_delivery_release(d);
	}
}

bool response_delivery_post(response_delivery_t *d, response_wire_t *wire, uint32_t timeout_ms,
							bool nonblocking)
{
	if (d->owned_wire == NULL) {
		response_delivery_own_wire(d, wire);
	}
	ZEND_ASSERT(d->owned_wire == wire);
	d->owned_ready = true;
	response_delivery_owner_t *o = d->owner;
	if (o == NULL) {
		response_delivery_discard_owned(d);
		return false;
	}
	delivery_gate_t *g = &o->gates[d->reactor];
	zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
	const uint64_t deadline = timeout_ms != 0 ? zend_hrtime() + (uint64_t)timeout_ms * 1000000 : 0;
	for (;;) {
		if (response_delivery_failed(d) || o->closing ||
			!reactor_pool_is_running(o->pool, d->reactor)) {
			break;
		}
		thread_queue_result_t result =
			reactor_pool_try_post_exec(o->pool, d->reactor, d->apply, wire);
		if (result == THREAD_QUEUE_ACCEPTED) {
			d->owned_wire = NULL;
			return true;
		}
		if (result != THREAD_QUEUE_FULL) {
			break;
		}
		if (nonblocking) {
			d->owned_ready = false; /* retryable offer, not a mandatory lost wire */
			response_delivery_discard_owned(d);
			return false; /* retryable offer, not a lost mandatory fragment */
		}
		if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
			break;
		}
		if (deadline != 0 && zend_hrtime() >= deadline) {
			break;
		}

		delivery_waiter_t w = {.delivery = d};
		if (!wait_prepare(&w, co, deadline)) {
			break;
		}
		gate_add(g, &w);
		/* Subscription is live before the second enqueue: a wake before
		 * SUSPEND resolves its waker instead of disappearing. */
		result = !response_delivery_failed(d) && !o->closing
					 ? reactor_pool_try_post_exec(o->pool, d->reactor, d->apply, wire)
					 : THREAD_QUEUE_STOPPED;
		bool posted = result == THREAD_QUEUE_ACCEPTED;
		if (!wait_finish(g, &w, co, d->reactor,
						 result == THREAD_QUEUE_FULL &&
							 reactor_pool_is_running(o->pool, d->reactor) &&
							 !response_delivery_failed(d))) {
			return false;
		}
		if (posted) {
			d->owned_wire = NULL;
			return true;
		}
		if (result != THREAD_QUEUE_FULL) {
			break;
		}
		if (EG(exception) != NULL) {
			break;
		}
	}
	response_delivery_cancel(d, "reverse_post_failed");
	response_delivery_discard_owned(d);
	return false;
}

void response_delivery_started(response_delivery_t *d, int status)
{
	if (d != NULL && zend_atomic_int_load_ex(&d->state) < DELIVERY_OK) {
		d->submitted_status = status > 0 ? status : 200;
	}
}

bool response_delivery_sendable(response_delivery_t *d)
{
	return d != NULL && d->owner != NULL && !d->owner->closing &&
		   zend_atomic_int_load_ex(&d->state) == DELIVERY_PENDING &&
		   reactor_pool_has_capacity(d->pool, d->reactor);
}

bool response_delivery_wait_writable(response_delivery_t *d, uint32_t timeout_ms)
{
	if (response_delivery_sendable(d)) {
		return true;
	}
	if (d->owner == NULL) {
		return false;
	}
	response_delivery_owner_t *o = d->owner;
	delivery_gate_t *g = &o->gates[d->reactor];
	zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
	if (co == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
		return false;
	}
	const uint64_t deadline = timeout_ms != 0 ? zend_hrtime() + (uint64_t)timeout_ms * 1000000 : 0;
	while (!response_delivery_sendable(d)) {
		if (zend_atomic_int_load_ex(&d->state) != DELIVERY_PENDING || o->closing ||
			!reactor_pool_is_running(d->pool, d->reactor)) {
			return false;
		}
		if (deadline != 0 && zend_hrtime() >= deadline) {
			return false;
		}
		delivery_waiter_t w = {.delivery = d};
		if (!wait_prepare(&w, co, deadline)) {
			return false;
		}
		gate_add(g, &w);
		if (!wait_finish(g, &w, co, d->reactor, !response_delivery_sendable(d))) {
			return false;
		}
		if (EG(exception) != NULL) {
			return false;
		}
	}
	return true;
}

void response_delivery_ack_transport(response_delivery_t *d, uint64_t bytes)
{
	if (d != NULL && bytes != 0 && zend_atomic_int_load_ex(&d->state) < DELIVERY_OK) {
		d->status = d->submitted_status;
	}
}

void response_delivery_ack(response_delivery_t *d, uint64_t bytes)
{
	if (d != NULL && zend_atomic_int_load_ex(&d->state) < DELIVERY_OK) {
		d->acked_body += bytes;
	}
}

void response_delivery_finish(response_delivery_t *d, bool clean, const char *reason)
{
	if (d == NULL) {
		return;
	}
	int state = zend_atomic_int_load_ex(&d->state);
	for (;;) {
		if (state == DELIVERY_OK || state == DELIVERY_ERROR) {
			return;
		}
		if (clean) {
			d->status = d->submitted_status; /* clean close confirms headers too */
		}
		int desired =
			clean && state == DELIVERY_PENDING && d->status != 0 ? DELIVERY_OK : DELIVERY_ERROR;
		if (zend_atomic_int_compare_exchange_ex(&d->state, &state, desired)) {
			break;
		}
	}
	d->reason = reason;
	response_delivery_owner_t *o = d->owner;
	delivery_publish(d, TERMINAL_READY);
	o->wake->trigger(o->wake); /* wake capacity waiters on peer close */
}

void response_delivery_wire_acquire(response_delivery_t *d)
{
	response_delivery_addref(d);
	zend_atomic_int_inc(&d->wires);
}

void response_delivery_own_wire(response_delivery_t *d, response_wire_t *wire)
{
	ZEND_ASSERT(d->owned_wire == NULL);
	d->owned_wire = wire;
	d->owned_ready = false;
	if (response_wire_delivery(wire) == NULL) {
		response_wire_set_delivery(wire, d);
	}
}
void response_delivery_discard_owned(response_delivery_t *d)
{
	response_wire_t *wire = d->owned_wire;
	d->owned_wire = NULL;
	if (wire != NULL) {
		if (d->owned_ready) {
			response_delivery_note_drop(d);
		}
		response_wire_discard(wire);
	}
}
void response_delivery_wire_release(response_delivery_t *d)
{
	if (zend_atomic_int_fetch_sub(&d->wires, 1) == 1) {
		delivery_publish(d, 0);
	}
	response_delivery_release(d);
}

void response_delivery_request_consumed(response_delivery_t *d)
{
	response_delivery_addref(d);
	if (!reactor_pool_post_control(d->pool, d->reactor, &d->consumed_node)) {
		response_delivery_release(d);
	}
}

void response_delivery_maybe_release_request(response_delivery_t *d)
{
	if (d->stream != NULL && d->request_consumed && !d->request_released &&
		zend_atomic_int_load_ex(&d->wires) == 0) {
		d->request_released = true;
		d->release_request(d->stream);
	}
}

void response_delivery_stream_gone(response_delivery_t *d)
{
	if (d == NULL) {
		return;
	}
	response_delivery_finish(d, false, "connection_closed");
	d->stream = NULL;
	response_delivery_release(d); /* reactor stream */
}

void response_delivery_owner_close(response_delivery_owner_t *o)
{
	if (o == NULL || o->closing) {
		return;
	}
	o->closing = true;
	zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;
	const bool can_suspend = co != NULL && !ZEND_ASYNC_IS_SCHEDULER_CONTEXT;
	for (response_delivery_t *d = o->active; d != NULL; d = d->active_next) {
		response_delivery_cancel(d, "worker_stopped");
		if (!can_suspend) {
			response_delivery_discard_owned(d);
			delivery_publish(d, SNAPSHOT_READY);
		}
	}
	if (!can_suspend) {
		/* No PHP coroutine can be awaited from a destructor/scheduler stack.
		 * Detach local stack subscriptions; their resumed cleanup must not
		 * touch an endpoint which has already been fenced and destroyed. */
		for (int i = 0; i < o->reactors; i++) {
			delivery_waiter_t *w = o->gates[i].waiters;
			o->gates[i].waiters = NULL;
			o->gates[i].tail = NULL;
			zend_atomic_bool_store_ex(&o->gates[i].waiting, false);
			while (w != NULL) {
				delivery_waiter_t *next = w->next;
				w->detached = true;
				async_plain_event_fire(w->event);
				w = next;
			}
		}
		o->waiters = 0;
		/* Scope teardown may be running on the scheduler stack. Cold fences
		 * acknowledge cancellations without trying to suspend a dead coroutine. */
		for (int i = 0; i < o->reactors; i++) {
			reactor_pool_control_exec(o->pool, i, capacity_wake, &o->gates[i]);
		}
		/* Data fences run after accepted wires of this producer. A control
		 * fence alone can overtake those wires and cannot prove reclamation. */
		for (int i = 0; i < o->reactors; i++) {
			reactor_pool_exec(o->pool, i, capacity_wake, &o->gates[i]);
		}
		owner_drain(o);
		ZEND_ASSERT(zend_atomic_int_load_ex(&o->outstanding) == 0);
	}
	while (zend_atomic_int_load_ex(&o->outstanding) != 0 || o->waiters != 0) {
		owner_drain(o);
		if (zend_atomic_int_load_ex(&o->outstanding) == 0 && o->waiters == 0) {
			break;
		}
		ZEND_ASYNC_WAKER_NEW(co);
		zend_async_resume_when(co, &o->wake->base, false, zend_async_waker_callback_resolve, NULL);
		if (http_atomic_pointer_load(&o->completed) == NULL) {
			ZEND_ASYNC_SUSPEND();
		}
		ZEND_ASYNC_WAKER_DESTROY(co);
		if (EG(exception) != NULL) {
			zend_clear_exception();
		}
	}
	for (int i = 0; i < o->reactors; i++) {
		reactor_pool_capacity_unregister(o->pool, i, &o->gates[i].gate);
	}
}

void response_delivery_owner_free(response_delivery_owner_t *o)
{
	if (o == NULL) {
		return;
	}
	ZEND_ASSERT(o->closing && zend_atomic_int_load_ex(&o->outstanding) == 0);
	ZEND_ASYNC_EVENT_SET_CLOSED(&o->wake->base);
	o->wake->base.dispose(&o->wake->base);
	free(o->gates);
	free(o);
}

void response_delivery_note_drop(response_delivery_t *d)
{
	if (d != NULL) {
		zend_atomic_int_inc(&d->dropped_wires);
	}
}
