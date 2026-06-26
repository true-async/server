--TEST--
Reactor pool H3 listener (#80, B3p3-a): UDP listener recv is serviced on the reactor thread
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_reactor_h3_listener_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
if (!class_exists('TrueAsync\\HttpServer') || !\TrueAsync\HttpServer::isHttp3()) {
    die('skip http_server built without HTTP/3 support');
}
if (PHP_OS_FAMILY === 'Windows') {
    die('skip reactor H3 listener spike is POSIX-only');
}
?>
--FILE--
<?php
/* B3p3-a: the first reactor-side step. Stand up a transport reactor, spawn the
 * H3 UDP listener ON that reactor's thread (its uv-bound socket + poll handle
 * must be created on the loop that owns them), fire a few datagrams at it from
 * this thread, and confirm the reactor's own loop counted them.
 *
 * Proves the core unknown of the split — the H3 listener can live and recv on a
 * pure-C transport reactor, not a PHP worker — with no PHP dispatch and no
 * crypto (server_obj == NULL, ssl_ctx == NULL). Dispatch to a worker by pointer
 * (B3p3-b) and the reverse path (B4) build on top of this. */

$ok = _http_server_reactor_h3_listener_selftest();

var_dump($ok);

echo "done\n";
?>
--EXPECT--
bool(true)
done
