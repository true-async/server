/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * The PHP object layer over the room core: the Room class, its transfer
 * handlers, and the HttpServer methods that address a room by name.
 *
 * A Room holds a hub reference and a topic, so it outlives the HttpServer that
 * minted it and travels into another thread through transfer_obj. The server
 * object itself is opaque here — this file reaches it through the pinpoint
 * accessors in php_http_server.h, the same way src/http3/ does.
 *
 * Nothing here is behind the WebSocket build flag: a build configured with
 * --disable-websocket registers the same classes and serves the same rooms. A
 * connection is one receiver among others (room/room_tree.h), and it is the
 * connection that is optional, not the room.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "php_http_server.h"
#include "room/php_room.h"
#include "../stubs/RoomExceptions.php_arginfo.h"
#include "Zend/zend_exceptions.h"

#include "room/room_hub.h"
#include "room/room_tree.h"
#include "../stubs/Room.php_arginfo.h"

/* {served, posted, dropped, workers} — publish()'s per-call delivery breakdown:
 * local subscribers served synchronously on the calling worker, remote mailboxes
 * that accepted the copy, full remote mailboxes that dropped it, and the worker
 * slots that were live at all — which is what tells a publish that reached nobody
 * from one that had nowhere to go. */
static void room_publish_result(zval *return_value, const room_hub_publish_result_t *r)
{
    array_init(return_value);
    add_assoc_long(return_value, "served",  (zend_long) r->served);
    add_assoc_long(return_value, "posted",  (zend_long) r->posted);
    add_assoc_long(return_value, "dropped", (zend_long) r->dropped);
    add_assoc_long(return_value, "workers", (zend_long) r->workers);
}

/* The per-call reliable-send knobs as configured; queue_max is per worker, in
 * entries. The drainer's cadence is not here: it belongs to the hub, which takes
 * it once at creation. */
typedef struct {
    uint32_t timeout_ms;    /* used when a call passes no timeout of its own */
    uint32_t queue_max;
} room_retry_cfg_t;

/* The knobs from the server's config, with the documented default per unset value. */
static room_retry_cfg_t room_retry_cfg(http_server_object *server)
{
    const http_server_config_t *const cfg = http_server_get_config(server);

    room_retry_cfg_t out;
    out.timeout_ms  = (cfg != NULL && cfg->ws_publish_retry_timeout_ms != 0)
        ? cfg->ws_publish_retry_timeout_ms : 5000u;
    out.queue_max   = (cfg != NULL && cfg->ws_publish_retry_queue_max != 0)
        ? cfg->ws_publish_retry_queue_max : 4096u;

    return out;
}

/* One call's knobs: a null timeoutMs means the configured default. */
static void room_retry_knobs(const room_retry_cfg_t *cfg, bool timeout_is_null,
        zend_long timeout_ms, uint32_t *timeout, uint32_t *queue_max)
{
    *queue_max = cfg->queue_max;

    if (timeout_is_null) {
        *timeout = cfg->timeout_ms;
    } else if (timeout_ms < 0) {
        *timeout = 0u;
    } else if ((zend_ulong) timeout_ms > UINT32_MAX) {
        *timeout = UINT32_MAX;   /* clamp both ends, matching the config setters' range */
    } else {
        *timeout = (uint32_t) timeout_ms;
    }
}

/* Throw RoomDeliveryException with its readonly delivered/pending props set — the
 * WebSocketClosedException one-time-init pattern (direct slot writes past the
 * readonly guard). */
static void room_throw_delivery(const char *msg, uint32_t delivered, uint32_t pending)
{
    zval ex;
    object_init_ex(&ex, room_delivery_exception_ce);
    zend_object *const obj = Z_OBJ(ex);

    zend_update_property_string(zend_ce_exception, obj, ZEND_STRL("message"), msg);

    const zend_property_info *const pd = zend_hash_str_find_ptr(
        &room_delivery_exception_ce->properties_info, ZEND_STRL("delivered"));
    zval *const slot_d = OBJ_PROP(obj, pd->offset);
    zval_ptr_dtor(slot_d);
    ZVAL_LONG(slot_d, delivered);

    const zend_property_info *const pp = zend_hash_str_find_ptr(
        &room_delivery_exception_ce->properties_info, ZEND_STRL("pending"));
    zval *const slot_p = OBJ_PROP(obj, pp->offset);
    zval_ptr_dtor(slot_p);
    ZVAL_LONG(slot_p, pending);

    zend_throw_exception_object(&ex);
}

/* Map a reliable-send result onto return_value (int = targets delivered, on OK)
 * or a thrown RoomDeliveryException carrying delivered/pending. A cancellation
 * resumed the sender with its own exception already pending — nothing to map and
 * nothing to add. */
static void room_send_apply(zval *return_value, const room_hub_send_result_t *r)
{
    switch (r->status) {
        case ROOM_HUB_SEND_OK:
            ZVAL_LONG(return_value, (zend_long) r->delivered);
            return;
        case ROOM_HUB_SEND_CANCELLED:
            return;
        case ROOM_HUB_SEND_NO_WORKERS:
            room_throw_delivery(
                "Reliable send reached nothing: no thread is attached to this room's hub, so the "
                "message had nowhere to go — the server is not running, or this is a thread that "
                "never joined it",
                r->delivered, r->pending);
            return;
        case ROOM_HUB_SEND_NO_TARGETS:
            room_throw_delivery(
                "Reliable send reached nothing: the workers are running, but nobody has subscribed "
                "to this room — use publish() for a message that may legitimately reach no one",
                r->delivered, r->pending);
            return;
        case ROOM_HUB_SEND_QUEUE_FULL:
            room_throw_delivery(
                "Reliable send refused: the outbound retry queue is at its cap "
                "(setWsPublishRetryQueueMax)",
                r->delivered, r->pending);
            return;
        case ROOM_HUB_SEND_NO_CONTEXT:
            room_throw_delivery(
                "Room::send() must be called from a coroutine; use trySend() outside one",
                r->delivered, r->pending);
            return;
        case ROOM_HUB_SEND_NO_QUEUE:
            room_throw_delivery(
                "Reliable send unavailable: this thread has no worker outbound queue to retry on",
                r->delivered, r->pending);
            return;
        case ROOM_HUB_SEND_SHUTDOWN:
            room_throw_delivery(
                "Reliable send interrupted: the worker shut down while the message was in flight",
                r->delivered, r->pending);
            return;
        case ROOM_HUB_SEND_EXPIRED:
        default:
            room_throw_delivery(
                "Reliable send timed out: a target mailbox was still full at the deadline",
                r->delivered, r->pending);
            return;
    }
}

/* {{{ proto HttpServer::enableRooms(): static
 * Opt into cross-worker rooms: allocate the topic hub up front so room
 * publishes work before start(). start() also creates it on demand, so this is
 * an explicit switch rather than a requirement. */
ZEND_METHOD(TrueAsync_HttpServer, enableRooms)
{
    ZEND_PARSE_PARAMETERS_NONE();

    http_server_object *server = http_server_object_from_zend(Z_OBJ_P(ZEND_THIS));

    if (http_server_is_running(server)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot enable rooms while the server is running", 0);
        return;
    }

    http_server_topic_hub_ensure(server);

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ proto HttpServer::publish(string $topic, string $message, bool $binary = false): array
 * Server-side publish to a room — no connection, so no sender is excluded; the
 * message reaches every subscriber of $topic on every worker. Returns the
 * per-call delivery breakdown (room_publish_result). */
ZEND_METHOD(TrueAsync_HttpServer, publish)
{
    zend_string *topic;
    zend_string *message;
    bool         binary = false;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(topic)
        Z_PARAM_STR(message)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(binary)
    ZEND_PARSE_PARAMETERS_END();

    http_server_object *server = http_server_object_from_zend(Z_OBJ_P(ZEND_THIS));

    if (http_server_get_topic_hub(server) == NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Rooms are not available: call enableRooms() before start()", 0);
        return;
    }

    if (!room_topic_is_valid_name(ZSTR_VAL(topic), ZSTR_LEN(topic))) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Invalid room name: a publish topic must be a concrete name "
            "(no '+' or '#' wildcards)", 0);
        return;
    }

    const room_hub_publish_result_t r = room_hub_publish(
        (room_hub_t *) http_server_get_topic_hub(server),
        ZSTR_VAL(topic), ZSTR_LEN(topic),
        ZSTR_VAL(message), ZSTR_LEN(message),
        binary,
        /* except_id: 0 = server origin, exclude nobody */ 0
    );

    room_publish_result(return_value, &r);
}
/* }}} */

/* {{{ proto HttpServer::trySend(string $topic, string $message, ?int $timeoutMs = null): bool
 * Reliable non-blocking send: fan out, park full targets on the outbound queue,
 * return at once. See Room::trySend(). */
ZEND_METHOD(TrueAsync_HttpServer, trySend)
{
    zend_string *topic;
    zend_string *message;
    zend_long    timeout_ms      = 0;
    bool         timeout_is_null = true;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(topic)
        Z_PARAM_STR(message)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(timeout_ms, timeout_is_null)
    ZEND_PARSE_PARAMETERS_END();

    http_server_object *server = http_server_object_from_zend(Z_OBJ_P(ZEND_THIS));

    if (http_server_get_topic_hub(server) == NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Rooms are not available: call enableRooms() before start()", 0);
        return;
    }

    if (!room_topic_is_valid_name(ZSTR_VAL(topic), ZSTR_LEN(topic))) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Invalid room name: a send topic must be a concrete name (no '+' or '#' wildcards)", 0);
        return;
    }

    uint32_t timeout, queue_max;
    const room_retry_cfg_t cfg = room_retry_cfg(server);
    room_retry_knobs(&cfg, timeout_is_null, timeout_ms, &timeout, &queue_max);

    const room_hub_send_result_t r = room_hub_try_send(
        (room_hub_t *) http_server_get_topic_hub(server),
        ZSTR_VAL(topic), ZSTR_LEN(topic),
        ZSTR_VAL(message), ZSTR_LEN(message),
        /* binary */ false, /* except_id */ 0,
        timeout, queue_max);

    RETURN_BOOL(r.status == ROOM_HUB_SEND_OK);
}
/* }}} */

/* {{{ proto HttpServer::send(string $topic, string $message, ?int $timeoutMs = null): int
 * Reliable blocking send: suspends until every target lands or the deadline
 * passes (then throws RoomDeliveryException). See Room::send(). */
ZEND_METHOD(TrueAsync_HttpServer, send)
{
    zend_string *topic;
    zend_string *message;
    zend_long    timeout_ms      = 0;
    bool         timeout_is_null = true;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(topic)
        Z_PARAM_STR(message)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(timeout_ms, timeout_is_null)
    ZEND_PARSE_PARAMETERS_END();

    http_server_object *server = http_server_object_from_zend(Z_OBJ_P(ZEND_THIS));

    if (http_server_get_topic_hub(server) == NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Rooms are not available: call enableRooms() before start()", 0);
        return;
    }

    if (!room_topic_is_valid_name(ZSTR_VAL(topic), ZSTR_LEN(topic))) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Invalid room name: a send topic must be a concrete name (no '+' or '#' wildcards)", 0);
        return;
    }

    uint32_t timeout, queue_max;
    const room_retry_cfg_t cfg = room_retry_cfg(server);
    room_retry_knobs(&cfg, timeout_is_null, timeout_ms, &timeout, &queue_max);

    const room_hub_send_result_t r = room_hub_send(
        (room_hub_t *) http_server_get_topic_hub(server),
        ZSTR_VAL(topic), ZSTR_LEN(topic),
        ZSTR_VAL(message), ZSTR_LEN(message),
        /* binary */ false, /* except_id */ 0,
        timeout, queue_max);

    room_send_apply(return_value, &r);
}
/* }}} */

/* {{{ proto HttpServer::subscriberCount(string $topic, int $timeoutMs = 1000): int
 * Cross-worker subscriber tally for a room (scatter/gather). Suspends the
 * calling coroutine; outside a coroutine it returns the local worker's count. */
ZEND_METHOD(TrueAsync_HttpServer, subscriberCount)
{
    zend_string *topic;
    zend_long    timeout_ms = 1000;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(topic)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(timeout_ms)
    ZEND_PARSE_PARAMETERS_END();

    http_server_object *server = http_server_object_from_zend(Z_OBJ_P(ZEND_THIS));

    if (http_server_get_topic_hub(server) == NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Rooms are not available: call enableRooms() before start()", 0);
        return;
    }

    if (!room_topic_is_valid_name(ZSTR_VAL(topic), ZSTR_LEN(topic))) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Invalid room name: a count topic must be a concrete name "
            "(no '+' or '#' wildcards)", 0);
        return;
    }

    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    const uint32_t count = room_hub_count(
        (room_hub_t *) http_server_get_topic_hub(server),
        ZSTR_VAL(topic), ZSTR_LEN(topic),
        (uint32_t) timeout_ms
    );

    RETURN_LONG((zend_long) count);
}
/* }}} */

/* ---- Room -----------------------------------------------------------------
 *
 * A server-side handle to a topic, minted by HttpServer::room(). It owns a hub
 * reference and the topic, publishes with no connection, and stays usable once
 * the HttpServer object is released. */
static zend_class_entry   *room_ce = NULL;
static zend_object_handlers room_handlers;

typedef struct {
    room_hub_t        *hub;     /* owns a reference; NULL only on an unminted object */
    zend_string       *topic;   /* owned; concrete name */
    room_retry_cfg_t   retry;   /* snapshot; the server's config is locked at construction */

    /* This thread's subscription, NULL until subscribe(). Never transferred:
     * a transferred room subscribes again in its new thread. */
    room_server_sub_t *sub;

    /* Losses from subscriptions this handle has already dropped, so lostCount()
     * stays monotonic across an unsubscribe/subscribe pair. */
    uint64_t           lost_before;

    zend_object        std;
} room_object;

static zend_always_inline room_object *room_from_obj(zend_object *obj)
{
    return (room_object *)((char *)obj - offsetof(room_object, std));
}

#define Z_ROOM_P(zv) room_from_obj(Z_OBJ_P(zv))

static zend_object *room_create(zend_class_entry *ce)
{
    room_object *obj = zend_object_alloc(sizeof(*obj), ce);

    obj->hub         = NULL;
    obj->topic       = NULL;
    obj->sub         = NULL;
    obj->lost_before = 0;
    memset(&obj->retry, 0, sizeof(obj->retry));

    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &room_handlers;

    return &obj->std;
}

static void room_free(zend_object *obj)
{
    room_object *room = room_from_obj(obj);

    if (room->sub != NULL) {
        room_hub_unsubscribe(room->sub);
        room_hub_sub_release(room->sub);
        room->sub = NULL;
    }

    if (room->topic != NULL) {
        zend_string_release(room->topic);
    }

    room_hub_release(room->hub);
    zend_object_std_dtor(&room->std);
}

/* Takes the hub reference; the topic arrives already validated. */
static zend_object *room_mint(http_server_object *server, zend_string *topic)
{
    zend_object *obj  = room_create(room_ce);
    room_object *room = room_from_obj(obj);

    room->hub = (room_hub_t *) http_server_get_topic_hub(server);
    room_hub_addref(room->hub);

    room->topic = zend_string_copy(topic);
    room->retry = room_retry_cfg(server);

    return obj;
}

/* Carries a room to another thread. The transit shell owns its own hub reference
 * and a persistent copy of the topic, so neither outlives the source thread's
 * allocator; LOAD takes a second reference and a thread-local copy. */
static zend_object *room_transfer_obj(
    zend_object *object,
    zend_async_thread_transfer_ctx_t *ctx,
    zend_object_transfer_kind_t kind,
    zend_object_transfer_default_fn default_fn)
{
    room_object *src = room_from_obj(object);

    if (kind == ZEND_OBJECT_TRANSFER_RELEASE) {
        room_hub_release(src->hub);
        src->hub = NULL;

        if (src->topic != NULL) {
            zend_string_release(src->topic);
            src->topic = NULL;
        }

        return NULL;
    }

    if (UNEXPECTED(src->hub == NULL)) {
        return NULL;   /* unminted; nothing to carry */
    }

    const bool persistent = (kind == ZEND_OBJECT_TRANSFER);

    /* 0 lets the default size the allocation from the handler offset and the
     * property count; a literal sizeof() would stop covering declared properties. */
    zend_object *dst = default_fn(object, ctx, 0);

    if (UNEXPECTED(dst == NULL)) {
        return NULL;
    }

    room_object *room = room_from_obj(dst);

    room->hub = src->hub;
    room_hub_addref(room->hub);
    room->topic = zend_string_init(ZSTR_VAL(src->topic), ZSTR_LEN(src->topic), persistent);
    room->retry = src->retry;
    room->sub   = NULL;   /* a subscription belongs to one thread and is not transferred */

    return dst;
}

/* Reflection can build a Room past the private constructor: hub and topic NULL. */
static bool room_is_minted(const room_object *room)
{
    if (UNEXPECTED(room->hub == NULL)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Room is uninitialized: rooms are minted by HttpServer::room()", 0);
        return false;
    }

    return true;
}

ZEND_METHOD(TrueAsync_Room, __construct)
{
    /* Private — rooms are minted by HttpServer::room(). */
    ZEND_PARSE_PARAMETERS_NONE();
}

/* {{{ proto Room::publish(string $message): array */
ZEND_METHOD(TrueAsync_Room, publish)
{
    zend_string *message;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(message)
    ZEND_PARSE_PARAMETERS_END();

    room_object *room = Z_ROOM_P(ZEND_THIS);

    if (!room_is_minted(room)) {
        return;
    }

    const room_hub_publish_result_t r = room_hub_publish(
        room->hub,
        ZSTR_VAL(room->topic), ZSTR_LEN(room->topic),
        ZSTR_VAL(message), ZSTR_LEN(message),
        /* binary */ false,
        /* except_id */ 0
    );

    room_publish_result(return_value, &r);
}
/* }}} */

/* {{{ proto Room::trySend(string $message, ?int $timeoutMs = null): bool */
ZEND_METHOD(TrueAsync_Room, trySend)
{
    zend_string *message;
    zend_long    timeout_ms      = 0;
    bool         timeout_is_null = true;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(message)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(timeout_ms, timeout_is_null)
    ZEND_PARSE_PARAMETERS_END();

    room_object *room = Z_ROOM_P(ZEND_THIS);

    if (!room_is_minted(room)) {
        return;
    }

    uint32_t timeout, queue_max;
    room_retry_knobs(&room->retry, timeout_is_null, timeout_ms, &timeout, &queue_max);

    const room_hub_send_result_t r = room_hub_try_send(
        room->hub,
        ZSTR_VAL(room->topic), ZSTR_LEN(room->topic),
        ZSTR_VAL(message), ZSTR_LEN(message),
        /* binary */ false, /* except_id */ 0,
        timeout, queue_max);

    RETURN_BOOL(r.status == ROOM_HUB_SEND_OK);
}
/* }}} */

/* {{{ proto Room::send(string $message, ?int $timeoutMs = null): int */
ZEND_METHOD(TrueAsync_Room, send)
{
    zend_string *message;
    zend_long    timeout_ms      = 0;
    bool         timeout_is_null = true;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(message)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(timeout_ms, timeout_is_null)
    ZEND_PARSE_PARAMETERS_END();

    room_object *room = Z_ROOM_P(ZEND_THIS);

    if (!room_is_minted(room)) {
        return;
    }

    uint32_t timeout, queue_max;
    room_retry_knobs(&room->retry, timeout_is_null, timeout_ms, &timeout, &queue_max);

    const room_hub_send_result_t r = room_hub_send(
        room->hub,
        ZSTR_VAL(room->topic), ZSTR_LEN(room->topic),
        ZSTR_VAL(message), ZSTR_LEN(message),
        /* binary */ false, /* except_id */ 0,
        timeout, queue_max);

    room_send_apply(return_value, &r);
}
/* }}} */

/* {{{ proto Room::publishBinary(string $data): int */
ZEND_METHOD(TrueAsync_Room, publishBinary)
{
    zend_string *data;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(data)
    ZEND_PARSE_PARAMETERS_END();

    room_object *room = Z_ROOM_P(ZEND_THIS);

    if (!room_is_minted(room)) {
        return;
    }

    const room_hub_publish_result_t r = room_hub_publish(
        room->hub,
        ZSTR_VAL(room->topic), ZSTR_LEN(room->topic),
        ZSTR_VAL(data), ZSTR_LEN(data),
        /* binary */ true,
        /* except_id */ 0
    );

    RETURN_LONG((zend_long) r.served);
}
/* }}} */

/* {{{ proto Room::subscriberCount(int $timeoutMs = 1000): int */
ZEND_METHOD(TrueAsync_Room, subscriberCount)
{
    zend_long timeout_ms = 1000;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(timeout_ms)
    ZEND_PARSE_PARAMETERS_END();

    room_object *room = Z_ROOM_P(ZEND_THIS);

    if (!room_is_minted(room)) {
        return;
    }

    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    const uint32_t count = room_hub_count(
        room->hub,
        ZSTR_VAL(room->topic), ZSTR_LEN(room->topic),
        (uint32_t) timeout_ms
    );

    RETURN_LONG((zend_long) count);
}
/* }}} */

/* {{{ proto Room::subscribe(): void */
ZEND_METHOD(TrueAsync_Room, subscribe)
{
    ZEND_PARSE_PARAMETERS_NONE();

    room_object *room = Z_ROOM_P(ZEND_THIS);

    if (!room_is_minted(room)) {
        return;
    }

    if (room->sub != NULL) {
        return;
    }

    room->sub = room_hub_subscribe(room->hub, room->topic);

    if (room->sub == NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot subscribe: this thread could not attach to the topic hub", 0);
    }
}
/* }}} */

/* {{{ proto Room::unsubscribe(): void */
ZEND_METHOD(TrueAsync_Room, unsubscribe)
{
    ZEND_PARSE_PARAMETERS_NONE();

    room_object *room = Z_ROOM_P(ZEND_THIS);

    if (!room_is_minted(room) || room->sub == NULL) {
        return;
    }

    room->lost_before += room_hub_sub_lost(room->sub);

    room_hub_unsubscribe(room->sub);
    room_hub_sub_release(room->sub);
    room->sub = NULL;
}
/* }}} */

/* {{{ proto Room::recv(?int $timeoutMs = null): ?string */
ZEND_METHOD(TrueAsync_Room, recv)
{
    zend_long timeout_ms      = 0;
    bool      timeout_is_null = true;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(timeout_ms, timeout_is_null)
    ZEND_PARSE_PARAMETERS_END();

    room_object *room = Z_ROOM_P(ZEND_THIS);

    if (!room_is_minted(room)) {
        return;
    }

    /* A missing subscription is a mistake, not a quiet null: the caller cannot
     * tell "nothing arrived" from "nobody ever joined". */
    if (room->sub == NULL) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Room::recv() needs subscribe() first, in this thread", 0);
        return;
    }

    room_payload_t *payload = NULL;

    /* Only null waits without a deadline. A negative argument is an expired
     * computed deadline and takes what is queued, so the signed tri-state stays
     * a C convention rather than a PHP one. */
    const int64_t timeout = timeout_is_null ? -1 : (timeout_ms > 0 ? (int64_t) timeout_ms : 0);

    const room_hub_recv_status_t status = room_hub_recv(room->sub, timeout, &payload);

    /* A message is never dropped for a pending cancellation: one taken off the
     * ring is returned here, and one that arrived during a cancelled park was
     * left in the ring for the next reader. */
    switch (status) {
        case ROOM_HUB_RECV_MESSAGE: {
            size_t len;
            bool   binary;
            const char *const data = room_hub_payload_data(payload, &len, &binary);

            RETVAL_STRINGL(data, len);
            room_hub_payload_release(payload);
            return;
        }

        case ROOM_HUB_RECV_BUSY:
            zend_throw_exception(http_server_runtime_exception_ce,
                "Room::recv() is already parked in another coroutine on this room", 0);
            return;

        case ROOM_HUB_RECV_CLOSED:
            zend_throw_exception(http_server_runtime_exception_ce,
                "Room::recv() was interrupted: the subscription closed", 0);
            return;

        case ROOM_HUB_RECV_CANCELLED:
            return;   /* the cancellation's own exception is already pending */

        case ROOM_HUB_RECV_TIMEOUT:
        default:
            RETURN_NULL();
    }
}
/* }}} */

/* {{{ proto Room::lostCount(): int */
ZEND_METHOD(TrueAsync_Room, lostCount)
{
    ZEND_PARSE_PARAMETERS_NONE();

    room_object *room = Z_ROOM_P(ZEND_THIS);

    if (!room_is_minted(room)) {
        return;
    }

    RETURN_LONG((zend_long) (room->lost_before + room_hub_sub_lost(room->sub)));
}
/* }}} */

/* {{{ proto Room::name(): string */
ZEND_METHOD(TrueAsync_Room, name)
{
    ZEND_PARSE_PARAMETERS_NONE();

    room_object *room = Z_ROOM_P(ZEND_THIS);

    if (!room_is_minted(room)) {
        return;
    }

    RETURN_STR_COPY(room->topic);
}
/* }}} */

/* {{{ proto HttpServer::room(string $topic): Room
 * A server-side handle to a room (topic) for publishing/counting without a
 * connection. $topic must be a concrete name. Before start() the hub is created
 * on demand; on a running server without one the call is refused. */
ZEND_METHOD(TrueAsync_HttpServer, room)
{
    zend_string *topic;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(topic)
    ZEND_PARSE_PARAMETERS_END();

    if (!room_topic_is_valid_name(ZSTR_VAL(topic), ZSTR_LEN(topic))) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "Invalid room name: a room topic must be a concrete name (no '+' or '#' wildcards)", 0);
        return;
    }

    http_server_object *server = http_server_object_from_zend(Z_OBJ_P(ZEND_THIS));

    if (http_server_get_topic_hub(server) == NULL) {
        if (http_server_is_running(server)) {
            zend_throw_exception(http_server_runtime_exception_ce,
                "Rooms are not available: call enableRooms() before start()", 0);
            return;
        }

        http_server_topic_hub_ensure(server);
    }

    RETURN_OBJ(room_mint(server, topic));
}
/* }}} */

/* Ordering: RoomDeliveryException extends HttpServerException, so MINIT must
 * have run http_server_exceptions_register() before this — it does, four steps
 * earlier (src/http_server.c). Room itself inherits nothing and orders against
 * nobody. */
zend_class_entry *room_delivery_exception_ce = NULL;

void php_room_minit(void)
{
    room_delivery_exception_ce =
        register_class_TrueAsync_RoomDeliveryException(http_server_exception_ce);

    room_ce = register_class_TrueAsync_Room();
    room_ce->create_object = room_create;

    memcpy(&room_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    room_handlers.offset       = offsetof(room_object, std);
    room_handlers.free_obj     = room_free;
    room_handlers.clone_obj    = NULL;
    room_handlers.transfer_obj = room_transfer_obj;

    /* LOAD has no live source object and resolves the handler by class name. */
    room_ce->default_object_handlers = &room_handlers;
}
