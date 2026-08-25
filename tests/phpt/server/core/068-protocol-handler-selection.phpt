--TEST--
HttpServer — an HTTP/2 request runs the HTTP/2 handler, not the general one
--EXTENSIONS--
true_async_server
true_async
sockets
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
if (!TrueAsync\HttpServer::isHttp2()) die('skip built without HTTP/2');
/* The request below is prior-knowledge HTTP/2, so the probe has to ask for
 * a curl built with nghttp2, and `command -v` is a shell builtin cmd.exe
 * does not have. */
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* addHttpHandler is the registration every protocol falls back to, and
 * addHttp2Handler overrides it for a connection that speaks HTTP/2. The pick
 * used to happen at accept, before a byte had been read, so it could only ever
 * take the general one: addHttp2Handler answered nothing on any server that
 * also called addHttpHandler, which is every server that serves both.
 *
 * The order of registration is deliberate — the general handler is added last,
 * so a pick that simply took the most recent registration would also pass the
 * first line and fail the second. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../h2/_h2_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())->addListener('127.0.0.1', $port));

$server->addHttp2Handler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('h2-handler');
});

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('general-handler');
});

spawn(function () use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);

        if ($fp) { fclose($fp); break; }

        usleep(20000);
    }

    $devnull = h2_dev_null();
    $ask = static function (string $opt) use ($port, $devnull) {
        return trim((string) shell_exec(
            "curl -s $opt http://127.0.0.1:$port/x 2>$devnull"));
    };

    echo 'h2c: ', $ask('--http2-prior-knowledge'), "\n";
    echo 'h1 : ', $ask('--http1.1'), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
h2c: h2-handler
h1 : general-handler
