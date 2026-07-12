/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "websocket/php_websocket.h"
#include "websocket/ws_hub.h"
#include "websocket/ws_session.h"

#include "stubs/WebSocketRoom.php_arginfo.h"

zend_class_entry *websocket_room_ce = NULL;

/* A worker that does not answer within this bound is left out of the tally
 * rather than hanging the caller. */
#define WS_ROOM_COUNT_TIMEOUT_MS 1000

static zend_object_handlers websocket_room_handlers;

typedef struct {
    ws_hub_t    *hub;
    ws_room_t   *room;   /* reference held for the object's lifetime */
    zend_object  std;
} websocket_room_object;

static zend_always_inline websocket_room_object *websocket_room_from_obj(zend_object *obj)
{
    return (websocket_room_object *)((char *)obj - offsetof(websocket_room_object, std));
}

#define Z_WEBSOCKET_ROOM_P(zv) websocket_room_from_obj(Z_OBJ_P(zv))

static zend_object *websocket_room_create(zend_class_entry *ce)
{
    websocket_room_object *obj = zend_object_alloc(sizeof(*obj), ce);

    obj->hub  = NULL;
    obj->room = NULL;

    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &websocket_room_handlers;

    return &obj->std;
}

static void websocket_room_free(zend_object *obj)
{
    websocket_room_object *wrapper = websocket_room_from_obj(obj);

    ws_room_release(wrapper->hub, wrapper->room);

    zend_object_std_dtor(&wrapper->std);
}

zend_object *websocket_room_object_create(void *hub, zend_string *name)
{
    ws_room_t *const room = ws_hub_room(hub, name);

    if (room == NULL) {
        zend_throw_exception_ex(websocket_exception_ce, 0,
            "Cannot create WebSocket room \"%s\"", ZSTR_VAL(name));
        return NULL;
    }

    zend_object *const obj = websocket_room_create(websocket_room_ce);
    websocket_room_object *const wrapper = websocket_room_from_obj(obj);

    wrapper->hub  = hub;
    wrapper->room = room;

    return obj;
}

/* The WebSocket handed to us is a PHP object of THIS worker, so its session
 * pointer never crosses a thread. The session is built lazily, so joining has
 * to commit the upgrade exactly like send()/recv() — a handler that joins
 * before its first I/O has no session yet. */
static ws_session_t *ws_room_session_of(const zval *ws_zv)
{
    websocket_object *const w = Z_WEBSOCKET_P((zval *)ws_zv);

    if (!w->committed && !ws_commit_upgrade(w, true)) {
        return NULL;   /* exception already set */
    }

    if (w->session == NULL || w->closed) {
        zend_throw_exception_ex(websocket_closed_exception_ce, 0,
            "WebSocket is closed");
        return NULL;
    }

    return w->session;
}

ZEND_METHOD(TrueAsync_WebSocketRoom, join)
{
    zval *ws_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ws_zv, websocket_ce)
    ZEND_PARSE_PARAMETERS_END();

    websocket_room_object *const room = Z_WEBSOCKET_ROOM_P(ZEND_THIS);

    ws_session_t *const session = ws_room_session_of(ws_zv);
    if (session == NULL) {
        RETURN_THROWS();
    }

    if (!ws_hub_join(room->room, session)) {
        zend_throw_exception_ex(websocket_exception_ce, 0,
            "Cannot join room \"%s\" — this worker is not attached to the room hub",
            ZSTR_VAL(ws_room_name(room->room)));
        RETURN_THROWS();
    }
}

ZEND_METHOD(TrueAsync_WebSocketRoom, leave)
{
    zval *ws_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ws_zv, websocket_ce)
    ZEND_PARSE_PARAMETERS_END();

    websocket_room_object *const room = Z_WEBSOCKET_ROOM_P(ZEND_THIS);
    websocket_object *const w = Z_WEBSOCKET_P(ws_zv);

    if (w->session != NULL) {
        (void)ws_hub_leave(room->room, w->session);
    }
}

static void ws_room_broadcast(INTERNAL_FUNCTION_PARAMETERS, const bool binary)
{
    zend_string *payload;
    zval *except_zv = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(payload)
        Z_PARAM_OPTIONAL
        Z_PARAM_OBJECT_OF_CLASS_OR_NULL(except_zv, websocket_ce)
    ZEND_PARSE_PARAMETERS_END();

    websocket_room_object *const room = Z_WEBSOCKET_ROOM_P(ZEND_THIS);

    uint64_t except_id = 0;
    if (except_zv != NULL) {
        const websocket_object *const w = Z_WEBSOCKET_P(except_zv);

        if (w->session != NULL) {
            except_id = w->session->ws_id;
        }
    }

    const uint32_t sent = ws_hub_broadcast(room->hub, room->room,
        ZSTR_VAL(payload), ZSTR_LEN(payload), binary, except_id);

    RETURN_LONG((zend_long)sent);
}

ZEND_METHOD(TrueAsync_WebSocketRoom, broadcast)
{
    ws_room_broadcast(INTERNAL_FUNCTION_PARAM_PASSTHRU, false);
}

ZEND_METHOD(TrueAsync_WebSocketRoom, broadcastBinary)
{
    ws_room_broadcast(INTERNAL_FUNCTION_PARAM_PASSTHRU, true);
}

ZEND_METHOD(TrueAsync_WebSocketRoom, count)
{
    ZEND_PARSE_PARAMETERS_NONE();

    const websocket_room_object *const room = Z_WEBSOCKET_ROOM_P(ZEND_THIS);

    RETURN_LONG((zend_long)ws_hub_count(room->hub, room->room, WS_ROOM_COUNT_TIMEOUT_MS));
}

ZEND_METHOD(TrueAsync_WebSocketRoom, getName)
{
    ZEND_PARSE_PARAMETERS_NONE();

    const websocket_room_object *const room = Z_WEBSOCKET_ROOM_P(ZEND_THIS);

    RETURN_STR_COPY(ws_room_name(room->room));
}

ZEND_METHOD(TrueAsync_WebSocketRoom, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();

    zend_throw_error(NULL, "WebSocketRoom cannot be constructed directly — "
                           "use HttpServer::room()");
}

/* A room captured by a handler closure rides into every worker with it. The hub
 * is process-wide, so each side takes its own reference to the same ws_room_t —
 * nothing about the room is copied. */
static zend_object *websocket_room_transfer_obj(
    zend_object *object,
    zend_async_thread_transfer_ctx_t *ctx,
    zend_object_transfer_kind_t kind,
    zend_object_transfer_default_fn default_fn)
{
    websocket_room_object *const src = websocket_room_from_obj(object);

    if (kind == ZEND_OBJECT_TRANSFER) {
        zend_object *const dst = default_fn(object, ctx, sizeof(websocket_room_object));

        if (UNEXPECTED(dst == NULL)) {
            return NULL;
        }

        /* The shell carries its own reference so the room cannot be retired
         * between the transfer and the worker loading it. */
        websocket_room_object *const shell = websocket_room_from_obj(dst);
        shell->hub  = src->hub;
        shell->room = ws_hub_room(src->hub, ws_room_name(src->room));

        return dst;
    }

    /* LOAD */
    zend_object *const dst = default_fn(object, ctx, 0);

    if (UNEXPECTED(dst == NULL)) {
        return NULL;
    }

    websocket_room_object *const loaded = websocket_room_from_obj(dst);
    loaded->hub  = src->hub;
    loaded->room = ws_hub_room(src->hub, ws_room_name(src->room));

    return dst;
}

void ws_room_class_register(void)
{
    websocket_room_ce = register_class_TrueAsync_WebSocketRoom();

    memcpy(&websocket_room_handlers, &std_object_handlers,
           sizeof(zend_object_handlers));
    websocket_room_handlers.offset       = offsetof(websocket_room_object, std);
    websocket_room_handlers.free_obj     = websocket_room_free;
    websocket_room_handlers.clone_obj    = NULL;
    websocket_room_handlers.transfer_obj = websocket_room_transfer_obj;

    websocket_room_ce->create_object = websocket_room_create;

    /* thread.c resolves the LOAD-side handler through the class, not the
     * object — without this a room captured by a closure never loads. */
    websocket_room_ce->default_object_handlers = &websocket_room_handlers;
}
