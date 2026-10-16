/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_RATE_LIMIT_PHP_H
#define HTTP_RATE_LIMIT_PHP_H

/* Request rate limit (#414), the server side: the TrueAsync\RateLimit class,
 * a server's frozen rules and per-instance buckets, the check the dispatch
 * sites run, and the refusal they send. The arithmetic is core/rate_limit.h.
 *
 * A rule added to a server is frozen into a persistent, atomically refcounted
 * snapshot, so a worker clone shares it as it shares static mounts. Buckets
 * are per server instance and allocated by the instance that serves, at
 * start(); a pool parent holds rules but no buckets. */

#include "php.h"
#include "core/rate_limit.h"

typedef struct http_server_object http_server_object;
typedef struct http_request_t http_request_t;
typedef struct http_server_counters_s http_server_counters_t;
typedef struct _http_server_config_t http_server_config_t;
struct http_log_state;

/* A server's rules and, once it serves, their buckets. Owned by the server. */
typedef struct http_rate_limits_s http_rate_limits_t;

extern zend_class_entry *http_rate_limit_ce;

void http_rate_limit_class_register(void);

/* HttpServer::addRateLimit(): freezes and locks `rule_obj` (a RateLimit) and
 * appends it to `*set`, creating the set on first use. Throws and returns
 * false when the name is taken or the server would exceed its bucket bound. */
bool http_rate_limits_add(http_rate_limits_t **set, zend_object *rule_obj);

/* Releases the rules and frees the buckets. NULL is a no-op. */
void http_rate_limits_free(http_rate_limits_t *set);

/* The rules a worker clone loads: a persistent copy holding a reference to
 * each frozen rule. NULL when there are none. */
void *http_rate_limits_transit_create(const http_rate_limits_t *set);
void  http_rate_limits_transit_release(void *transit);

/* The clone's own set over the transit's rules, without buckets. */
http_rate_limits_t *http_rate_limits_load(const void *transit);

/* Allocates the buckets and the hash key of a serving instance, once; later
 * calls keep both. No-op on NULL. */
void http_rate_limits_prepare(http_rate_limits_t *set);

/* The start() notice: a Unix listener whose peers all share the key `unix`
 * because `unix` is not a trusted proxy. */
void http_rate_limits_log_start(const http_rate_limits_t *set, const http_server_config_t *config,
                                struct http_log_state *log_state);

/* getTelemetry()'s `rate_limits`: name => [limited, evictions] for this
 * instance. Adds nothing when the server has no rules. */
void http_rate_limits_telemetry(const http_rate_limits_t *set, zval *telemetry);

/* resetTelemetry(): zeroes the per-rule counts. NULL is a no-op. */
void http_rate_limits_telemetry_reset(http_rate_limits_t *set);

/* Whether the server has rules that will check requests. */
bool http_rate_limits_active(const http_server_object *server);

/* Checks `req` against the server's rules and takes its tokens when all admit
 * it. Returns 0 when admitted, else the Retry-After in seconds. Counts the
 * refusal and any evictions in `counters` and the per-rule stats at once, so
 * the caller answers a refusal with 429 straight away. Reads the request's
 * peer, method, target and X-Forwarded-For only. */
uint32_t http_rate_limit_check(http_server_object *server, const http_request_t *req,
                               http_server_counters_t *counters);

/* The same check for a verdict sent later (HTTP/1, before the body): an
 * admission and its evictions count at once, a refusal is kept on the request
 * (req->rate_limit_pending) and counts only through
 * http_rate_limit_count_refusal, where its 429 is sent. A request answered
 * otherwise (413) frees it uncounted with the request. */
uint32_t http_rate_limit_check_deferred(http_server_object *server, http_request_t *req,
                                        http_server_counters_t *counters);

/* Counts the refusal kept by http_rate_limit_check_deferred; no-op without one. */
void http_rate_limit_count_refusal(http_server_object *server, http_request_t *req,
                                   http_server_counters_t *counters);

/* Turns a fresh response into the refusal: 429, Retry-After and a short body;
 * or, for a gRPC call (`grpc`), status 200 with the trailers grpc-status 8,
 * grpc-message and grpc-retry-pushback-ms, which the gRPC finish path sends
 * as Trailers-Only. */
void http_rate_limit_refuse(zend_object *response, uint32_t retry_after_s, bool grpc);

#endif /* HTTP_RATE_LIMIT_PHP_H */
