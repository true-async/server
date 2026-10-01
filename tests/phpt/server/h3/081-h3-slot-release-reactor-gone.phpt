--TEST--
HttpServer: an HTTP/3 slot release bound for a reactor that has left its loop is dropped, not retried forever
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
if (!function_exists('_http_fault_enable')) die('skip needs --enable-fault-injection');
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* A worker returns an HTTP/3 request's slot by posting the release to the
 * reactor that owns it, and retries a post its mailbox refuses. A reactor that
 * has left its loop refuses every post, so the release is dropped with one
 * notice instead of spinning the worker forever. The fault point reports the
 * reactor as gone; its hit count proves the drop path ran. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$tmp = __DIR__ . '/tmp-081';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink("$tmp/stderr.txt"); @unlink($cert); @unlink($key); @rmdir($tmp);
});

$point = 'h3/slot_release/reactor_gone';
$port = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2));

$server->addHttpHandler(function ($req, $res) {
    $res->setBody('ok');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin, $tmp, $point) {
    /* Reactors + workers need a moment to thread up and bind. */
    delay(600);
    _http_fault_enable($point);

    $cmd = sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d / GET 2>%s',
        escapeshellarg($client_bin), $port, escapeshellarg("$tmp/stderr.txt"));
    $body = (string) shell_exec($cmd);
    preg_match('/^STATUS=(\d+)$/m', (string) @file_get_contents("$tmp/stderr.txt"), $m);

    /* The release runs once the worker frees the request, after the response. */
    delay(200);
    $hits = _http_fault_hits($point);
    _http_fault_disable($point);

    echo "response: ", $m[1] ?? '-', " $body\n";
    echo "fault hit: ", $hits > 0 ? 'yes' : 'no', "\n";

    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
%AHTTP/3 slot release dropped: reactor %d has left its loop (later drops are not reported)
%Aresponse: 200 ok
fault hit: yes
%Adone
