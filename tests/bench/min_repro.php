<?php
/* Minimum reproducer for the top-level-start dispatch crash.
 *
 * Run with:
 *   php -d extension_dir=$(pwd)/modules -d extension=true_async_server \
 *       tests/bench/min_repro.php
 *
 * In another shell:
 *   curl http://127.0.0.1:18081/
 *
 * Expected: "OK\n"
 * Actual:   AddressSanitizer SEGV in http_connection_dispatch_request
 *           (wild jump inside ZEND_ASYNC_NEW_COROUTINE).
 */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$config = (new HttpServerConfig())->addListener('127.0.0.1', 18081);
$server = new HttpServer($config);
$server->addHttpHandler(fn($req, $resp) => $resp->setBody('OK'));
$server->start();
