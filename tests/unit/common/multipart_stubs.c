/*
 * Stubs for PHP-dependent functions used in unit tests
 */

#include "formats/multipart_processor.h"
#include "php.h"
#include "Zend/zend_string.h"
#include "log/http_log.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>

/* Forward decl — avoid pulling the full php_http_server.h which drags the
 * whole connection layer in transitively. */
struct http_server_object;
typedef struct http_server_object http_server_object;
struct http_request_t;

/* Stub for uploaded_file_create_from_info - returns NULL in unit tests */
void* uploaded_file_create_from_info(mp_file_info_t *info)
{
    (void)info;
    return NULL;
}

/* Stubs for http_known_{method,header}_lookup — return NULL so the
 * parser falls back to zend_string_init (works the same in tests;
 * interning is just an allocation optimization). */
zend_string *http_known_method_lookup(const char *name, size_t len)
{
    (void)name;
    (void)len;
    return NULL;
}

zend_string *http_known_header_lookup(const char *name, size_t len)
{
    (void)name;
    (void)len;
    return NULL;
}

/* Admission-control stub. Tests don't drive backpressure; never shed.
 * http_server_on_request_shed migrated to a static-inline helper in
 * php_http_server.h, so no extern stub needed. */
bool http_server_should_shed_request(const http_server_object *server)
{
    (void)server;
    return false;
}

/* Logging is fired via http_logf_* macros that gate on log_state->severity;
 * tests pass a zero-initialized state (severity == HTTP_LOG_OFF) so the
 * macro short-circuits and this body is never actually entered. The
 * symbol still needs to resolve at link time, hence the no-op. */
void http_log_emitf(http_log_state_t *state, http_log_severity_t severity,
                    const http_log_attr_t *attrs, size_t nattrs,
                    const char *tmpl, ...)
{
    (void)state; (void)severity; (void)attrs; (void)nattrs; (void)tmpl;
}

/* Trace-context propagation is parsed from the `traceparent` header into
 * req->trace_*. Tests don't assert these fields, so the no-op leaves
 * them at zero (which is the documented "no incoming context" state). */
void http_request_parse_trace_context(struct http_request_t *req)
{
    (void)req;
}
