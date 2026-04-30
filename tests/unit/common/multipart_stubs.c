/*
 * Stubs for PHP-dependent functions used in unit tests
 */

#include "formats/multipart_processor.h"
#include "php.h"
#include "Zend/zend_string.h"
#include <stdlib.h>
#include <stdbool.h>

/* Forward decl — avoid pulling the full php_http_server.h which drags the
 * whole connection layer in transitively. */
struct http_server_object;
typedef struct http_server_object http_server_object;

/* Stub for uploaded_file_create_from_info - returns NULL in unit tests */
void* uploaded_file_create_from_info(mp_file_info_t *info)
{
    (void)info;
    return NULL;
}

/* Stub for http_known_method_lookup — return NULL so the parser falls
 * back to zend_string_init for the request method (works the same in
 * tests; interning is just an allocation optimization). */
zend_string *http_known_method_lookup(const char *name, size_t len)
{
    (void)name;
    (void)len;
    return NULL;
}

/* Admission-control stubs. Tests don't drive backpressure; never shed. */
bool http_server_should_shed_request(const http_server_object *server)
{
    (void)server;
    return false;
}

void http_server_on_request_shed(http_server_object *server, bool is_h2)
{
    (void)server;
    (void)is_h2;
}
