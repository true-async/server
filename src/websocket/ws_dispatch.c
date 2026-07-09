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
#include "Zend/zend_async_API.h"
#include "Zend/zend_exceptions.h"

#include "websocket/ws_dispatch.h"
#include "websocket/ws_handshake.h"
#include "websocket/ws_session.h"
#include "websocket/websocket_strategy.h"
#include "websocket/php_websocket.h"

#include "core/http_connection.h"
#include "core/http_connection_internal.h"  /* http_connection_tls_fsm_send_plaintext_atomic */
#include "core/http_protocol_strategy.h"
#include "core/http_protocol_handlers.h"
#include "http1/http_parser.h"

#include <string.h>

extern zval *http_request_create_from_parsed(http_request_t *req);

/*
 * Per-handler-coroutine context.
 *
 * The new (deferred-101) flow: try_upgrade spawns this coroutine
 * immediately, BEFORE any 101 has been sent. The handler may run
 * arbitrary code, possibly mutating the WebSocketUpgrade object
 * (reject / setSubprotocol). The first WS I/O method (recv / send
 * / close) auto-commits via ws_commit_upgrade — that is where the
 * 101 actually goes on the wire and the WS strategy installs.
 *
 * Holds three zvals: the WebSocket the handler operates on, the
 * upgrade HttpRequest passed as the second argument, and the
 * WebSocketUpgrade passed as the (optional, ignored on 2-arg
 * handlers) third argument. PHP's closures silently drop excess
 * positional args, so we always pass three — no Reflection arity
 * dance required.
 */
typedef struct {
    http_connection_t *conn;
    zval               request_zv;
    zval               websocket_zv;
    zval               upgrade_zv;
} ws_handler_ctx_t;

/* {{{ Forward decls */
static void ws_handler_coroutine_entry(void);
static void ws_handler_coroutine_dispose(zend_coroutine_t *coroutine);

static void ws_pending_write_complete_cb(zend_async_event_t *event,
                                         zend_async_event_callback_t *callback,
                                         void *result,
                                         zend_object *exception);
static void ws_pending_write_dispose(zend_async_event_callback_t *callback,
                                     zend_async_event_t *event);

static bool ws_submit_reject_write(http_connection_t *conn,
                                   char *buf, size_t len);

static zend_string *build_error_response(int status, const char *extra_header);
/* }}} */

/* Reject-path async write context. The reject flow lives entirely
 * in event-loop context: ws_dispatch_try_upgrade builds a 4xx,
 * submits an async write, and this callback frees the buffer when
 * the write completes. No coroutine ever runs on the reject path. */
typedef struct {
    zend_async_event_callback_t base;
    http_connection_t          *conn;
    zend_async_io_req_t        *active_req;
    char                       *buf;
} ws_pending_write_t;

/* Destroy a reject-path request, severing the parser's borrow first
 * (a later read tick must not see it freed). */
static void ws_reject_destroy_request(http_connection_t *conn, http_request_t *req)
{
    if (conn->parser != NULL) {
        http_parser_clear_request(conn->parser);
    }
    http_request_destroy(req);
}

bool ws_dispatch_try_upgrade(http_connection_t *conn, http_request_t *req)
{
    /* Cheapest probe first — no Upgrade header → not our concern. */
    const ws_handshake_result_t v = ws_handshake_validate(req);
    if (v == WS_HANDSHAKE_NOT_AN_UPGRADE) {
        return false;
    }

    HashTable *const handlers =
        http_server_get_protocol_handlers(conn->server);
    zend_fcall_t *const handler = handlers != NULL
        ? http_protocol_get_handler(handlers, HTTP_PROTOCOL_WEBSOCKET)
        : NULL;

    /* No WS handler registered → 426. */
    if (handler == NULL) {
        zend_string *resp = build_error_response(426,
            "Sec-WebSocket-Version: 13\r\n");
        if (resp == NULL) { conn->keep_alive = false; ws_reject_destroy_request(conn, req); return true; }
        char *buf = emalloc(ZSTR_LEN(resp));
        memcpy(buf, ZSTR_VAL(resp), ZSTR_LEN(resp));
        size_t len = ZSTR_LEN(resp);
        zend_string_release(resp);
        if (!ws_submit_reject_write(conn, buf, len)) {
            conn->keep_alive = false;
        }

        /* Reject path owns req (no HttpRequest object wraps it here). */
        ws_reject_destroy_request(conn, req);
        return true;
    }

    /* Validation failure → matching error code. */
    if (v < 0) {
        int status = 400;
        const char *extra = NULL;
        switch (v) {
            case WS_HANDSHAKE_FORBIDDEN_METHOD:   status = 405; extra = "Allow: GET\r\n"; break;
            case WS_HANDSHAKE_UPGRADE_REQUIRED:   status = 426; extra = "Sec-WebSocket-Version: 13\r\n"; break;
            case WS_HANDSHAKE_BAD_REQUEST:
            default:                              status = 400; extra = NULL; break;
        }
        zend_string *resp = build_error_response(status, extra);
        if (resp == NULL) { conn->keep_alive = false; ws_reject_destroy_request(conn, req); return true; }
        char *buf = emalloc(ZSTR_LEN(resp));
        memcpy(buf, ZSTR_VAL(resp), ZSTR_LEN(resp));
        size_t len = ZSTR_LEN(resp);
        zend_string_release(resp);
        if (!ws_submit_reject_write(conn, buf, len)) {
            conn->keep_alive = false;
        }

        /* Reject path owns req (no HttpRequest object wraps it here). */
        ws_reject_destroy_request(conn, req);
        return true;
    }

    /* Accept path — pre-compute Sec-WebSocket-Accept now (pure CPU,
     * no I/O) so the deferred commit step doesn't need to keep the
     * raw client-key around. */
    zval *const key_zv = zend_hash_str_find(req->headers,
        "sec-websocket-key", sizeof("sec-websocket-key") - 1);
    if (UNEXPECTED(key_zv == NULL || Z_TYPE_P(key_zv) != IS_STRING)) {
        conn->keep_alive = false;
        ws_reject_destroy_request(conn, req);
        return true;
    }
    char accept_value[WS_ACCEPT_LEN];
    if (ws_handshake_compute_accept(Z_STRVAL_P(key_zv), Z_STRLEN_P(key_zv),
                                    accept_value) != 0) {
        conn->keep_alive = false;
        ws_reject_destroy_request(conn, req);
        return true;
    }

    /* Build the PHP objects the handler will see and spawn the
     * coroutine. The WebSocket starts in pre-commit state — its
     * recv/send/close prologue calls ws_commit_upgrade on first use,
     * which is when the 101 actually goes on the wire. */
    ws_handler_ctx_t *ctx = ecalloc(1, sizeof(*ctx));
    ctx->conn = conn;

    zval *req_obj = http_request_create_from_parsed(req);
    ZVAL_COPY_VALUE(&ctx->request_zv, req_obj);
    efree(req_obj);

    zend_object *ws_obj  = websocket_object_create_pre_commit(conn,
                                                              accept_value);
    ZVAL_OBJ(&ctx->websocket_zv, ws_obj);

    zend_object *up_obj  = websocket_upgrade_object_create(conn, req);
    ZVAL_OBJ(&ctx->upgrade_zv, up_obj);

    /* Cross-link: the WebSocket needs to read the upgrade's
     * subprotocol/reject decision at commit time, and the upgrade
     * object holds a back-pointer to the WebSocket so reject() can
     * mark it. The websocket holds an owning ref (addref). */
    websocket_object *w = websocket_from_obj(ws_obj);
    Z_ADDREF(ctx->upgrade_zv);
    ZVAL_COPY_VALUE(&w->upgrade_zv, &ctx->upgrade_zv);
    websocket_upgrade_from_obj(up_obj)->ws = ws_obj;

    /* Spawn the handler in a per-request child scope via the shared
     * helper — the same lifecycle the H1/H2 handlers use, so the
     * server-stop drain (#74) cancels a recv()-suspended WS handler
     * cleanly instead of leaving the reactor loop alive. */
    zend_coroutine_t *coroutine = http_request_handler_coroutine_new(
        conn->scope, ws_handler_coroutine_entry, ctx,
        ws_handler_coroutine_dispose,
        conn->view != NULL ? conn->view->request_scope : true);
    if (coroutine == NULL) {
        zval_ptr_dtor(&ctx->request_zv);
        zval_ptr_dtor(&ctx->websocket_zv);
        zval_ptr_dtor(&ctx->upgrade_zv);
        efree(ctx);
        conn->keep_alive = false;
        /* req was freed with request_zv; sever the parser's borrow. */
        if (conn->parser != NULL) {
            http_parser_clear_request(conn->parser);
        }
        return true;
    }

    conn->handler = handler;
    conn->handler_refcount++;

    ZEND_ASYNC_ENQUEUE_COROUTINE(coroutine);
    return true;
}

/* {{{ build_error_response */
static zend_string *build_error_response(int status, const char *extra_header)
{
    const char *reason;
    switch (status) {
        case 400: reason = "Bad Request";        break;
        case 401: reason = "Unauthorized";       break;
        case 403: reason = "Forbidden";          break;
        case 405: reason = "Method Not Allowed"; break;
        case 426: reason = "Upgrade Required";   break;
        default:  reason = "Bad Request";        break;
    }

    smart_str buf = {0};
    smart_str_alloc(&buf, 256, 0);

    char status_line[64];
    int n = snprintf(status_line, sizeof(status_line),
                     "HTTP/1.1 %d %s\r\n", status, reason);
    smart_str_appendl(&buf, status_line, (size_t)n);
    smart_str_appends(&buf, "Connection: close\r\n");
    smart_str_appends(&buf, "Content-Type: text/plain; charset=utf-8\r\n");
    if (extra_header != NULL) {
        smart_str_appends(&buf, extra_header);
    }

    char content_length[48];
    int rl = (int)strlen(reason);
    n = snprintf(content_length, sizeof(content_length),
                 "Content-Length: %d\r\n\r\n", rl);
    smart_str_appendl(&buf, content_length, (size_t)n);
    smart_str_appendl(&buf, reason, (size_t)rl);
    smart_str_0(&buf);

    return buf.s;
}
/* }}} */

/* {{{ ws_submit_reject_write
 *
 * Queue a 4xx write from event-loop context (no coroutine). Used by
 * try_upgrade for the immediate-reject paths and by handler dispose
 * for the upgrade-was-rejected path. Takes ownership of `buf`.
 */
static bool ws_submit_reject_write(http_connection_t *conn,
                                   char *buf, size_t len)
{
#ifdef HAVE_OPENSSL
    /* Over TLS the raw IO_WRITE below would ship plaintext. Encrypt the
     * 4xx via the FSM's non-suspending atomic send (we are in event-loop
     * context — no coroutine to drive the suspending tls_push), then mark
     * the connection for close. */
    if (conn->tls != NULL) {
        const bool ok = http_connection_tls_fsm_send_plaintext_atomic(conn, buf, len);
        efree(buf);
        conn->keep_alive = false;
        return ok;
    }
#endif

    ws_pending_write_t *pw = (ws_pending_write_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(ws_pending_write_complete_cb,
                                     sizeof(ws_pending_write_t));
    if (pw == NULL) {
        efree(buf);
        return false;
    }
    pw->base.dispose = ws_pending_write_dispose;
    pw->conn         = conn;
    pw->buf          = buf;
    pw->active_req   = ZEND_ASYNC_IO_WRITE(conn->io, buf, len);
    if (pw->active_req == NULL) {
        efree(pw); efree(buf); return false;
    }
    if (!conn->io->event.add_callback(&conn->io->event, &pw->base)) {
        pw->active_req->dispose(pw->active_req);
        efree(pw); efree(buf);
        return false;
    }
    return true;
}
/* }}} */

static void ws_pending_write_complete_cb(
    zend_async_event_t *event,
    zend_async_event_callback_t *callback,
    void *result,
    zend_object *exception)
{
    (void)event;
    ws_pending_write_t *pw = (ws_pending_write_t *)callback;
    if (pw->active_req == NULL || result != pw->active_req) {
        return;
    }
    http_connection_t *conn = pw->conn;
    zend_async_io_req_t *req_io = pw->active_req;
    pw->active_req = NULL;

    (void)conn->io->event.del_callback(&conn->io->event, &pw->base);

    if (req_io->exception != NULL) {
        OBJ_RELEASE(req_io->exception);
        req_io->exception = NULL;
    }
    req_io->dispose(req_io);
    if (pw->buf) { efree(pw->buf); pw->buf = NULL; }

    /* Reject path always closes. */
    conn->keep_alive = false;

    efree(pw);
    (void)exception;
}

static void ws_pending_write_dispose(
    zend_async_event_callback_t *callback, zend_async_event_t *event)
{
    (void)callback; (void)event;
}

/* {{{ ws_handler_coroutine_entry
 *
 * Invoke the user PHP handler with three positional args:
 *   ($ws, $req, $upgrade)
 * 2-arg closures silently drop $upgrade (PHP closure semantics).
 * The handler runs entirely BEFORE the 101 has been sent — auto-commit
 * fires from the prologue of the first WS I/O method (recv/send/close)
 * the handler invokes.
 */
static void ws_handler_coroutine_entry(void)
{
    const zend_coroutine_t *coroutine = ZEND_ASYNC_CURRENT_COROUTINE;
    ws_handler_ctx_t *ctx = (ws_handler_ctx_t *)coroutine->extended_data;
    http_connection_t *conn = ctx->conn;

    zval params[3], retval;
    ZVAL_COPY_VALUE(&params[0], &ctx->websocket_zv);
    ZVAL_COPY_VALUE(&params[1], &ctx->request_zv);
    ZVAL_COPY_VALUE(&params[2], &ctx->upgrade_zv);
    ZVAL_UNDEF(&retval);

    call_user_function(NULL, NULL, &conn->handler->fci.function_name,
                       &retval, 3, params);

    zval_ptr_dtor(&retval);
}
/* }}} */

/* {{{ ws_handler_coroutine_dispose
 *
 * End-of-handler bookkeeping. Three paths:
 *
 *   1. Handler did some WS I/O → ws_commit_upgrade already fired
 *      somewhere inside its body. Nothing to do here besides flag
 *      the connection for close and decrement the pin.
 *
 *   2. Handler called WebSocketUpgrade::reject(N) and exited.
 *      Send the corresponding 4xx response asynchronously (we are
 *      in coroutine dispose context, but the write itself is
 *      submitted as a non-blocking IO_WRITE + completion callback —
 *      no need to suspend the dispose). Then close.
 *
 *   3. Handler returned without doing any WS I/O AND without
 *      rejecting (e.g. an empty 2-arg handler). Auto-commit the
 *      upgrade: send 101 (suspending — dispose runs in coroutine
 *      context just like h1 dispose does), then close. The peer
 *      sees a successful upgrade followed by an immediate close,
 *      which is well-defined behaviour.
 */
static void ws_handler_coroutine_dispose(zend_coroutine_t *coroutine)
{
    ws_handler_ctx_t *ctx = (ws_handler_ctx_t *)coroutine->extended_data;
    if (ctx == NULL) {
        return;
    }
    http_connection_t *conn = ctx->conn;
    coroutine->extended_data = NULL;

    websocket_object         *w = NULL;
    websocket_upgrade_object *u = NULL;
    if (Z_TYPE(ctx->websocket_zv) == IS_OBJECT) {
        w = websocket_from_obj(Z_OBJ(ctx->websocket_zv));
    }
    if (Z_TYPE(ctx->upgrade_zv) == IS_OBJECT) {
        u = websocket_upgrade_from_obj(Z_OBJ(ctx->upgrade_zv));
    }

    /* Case 2: reject was called pre-commit. Send the 4xx async. */
    if (u != NULL && !w->committed && u->reject_status != 0) {
        zend_string *resp = build_error_response(u->reject_status, NULL);
        if (resp != NULL) {
            char *buf = emalloc(ZSTR_LEN(resp));
            memcpy(buf, ZSTR_VAL(resp), ZSTR_LEN(resp));
            size_t len = ZSTR_LEN(resp);
            zend_string_release(resp);
            (void)ws_submit_reject_write(conn, buf, len);
        }
    }
    /* Case 3: nothing happened; auto-commit so the client at least
     * sees the 101 before close. ws_commit_upgrade is suspending,
     * which is allowed in dispose (h1 dispose also calls suspending
     * I/O). */
    else if (w != NULL && !w->committed) {
        (void)ws_commit_upgrade(w, false);
        if (EG(exception) != NULL) {
            zend_clear_exception();
        }
    }

    /* Mark the WebSocket as closed regardless of path. */
    if (w != NULL) {
        w->closed  = true;
        w->session = NULL;
    }

    /* capture before the dtor below frees w — reading it later is a UAF */
    const bool w_committed = (w != NULL) && w->committed;

    zval_ptr_dtor(&ctx->request_zv);
    zval_ptr_dtor(&ctx->websocket_zv);
    zval_ptr_dtor(&ctx->upgrade_zv);
    efree(ctx);

    conn->keep_alive = false;
    conn->current_request = NULL;

    /* the dtor above may have freed the request the parser still borrows —
     * sever it or a later read tick reads it freed */
    if (conn->parser != NULL) {
        http_parser_clear_request(conn->parser);
    }

    ZEND_ASSERT(conn->handler_refcount > 0);
    conn->handler_refcount--;

    /* Last reference on a COMMITTED WS connection: tear it down so its
     * read poll, wslay session, and keepalive ping timer are released —
     * a committed WS connection has no other teardown trigger once the
     * handler exits, and leaving those reactor handles armed trips the
     * loop-alive assertion at server stop. Mirrors http_request_finalize's
     * !keep_alive destroy.
     *
     * NOT for the reject / never-committed path: there an async 4xx write
     * is in flight and its completion callback (ws_pending_write_complete_cb)
     * owns teardown — destroying here would free conn under that write. */
    if (w_committed) {
        if (conn->handler_refcount == 0) {
            http_connection_destroy(conn);
        } else {
            /* A flusher pin is still held (e.g. a user-spawned writer
             * coroutine suspended inside wslay_event_send). Mark the
             * teardown pending; releasing the last pin runs it via
             * http_connection_destroy_if_idle_deferred. */
            conn->destroy_pending = true;
        }
    }
}
/* }}} */
