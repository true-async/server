--TEST--
HttpServer: HTTP/3 end-to-end GET through the reactor/worker split (#80, gated pool)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* Reactor-pool end-to-end (#80, B3p3-b + B4).
 *
 * With TRUE_ASYNC_SERVER_REACTOR_POOL=1 and setWorkers(2), the parent
 * spawns C-only transport reactors that own the H3 listeners and route
 * each parsed request to a PHP worker by pointer; the worker runs the
 * handler and posts the rendered response back to the reactor, which
 * QPACK-encodes and sends it. This locks in the first full split e2e
 * (previously only verified manually): the request method+uri must reach
 * the worker and the worker's status+body must reach the client.
 *
 * Multi-worker scope: the gated pool only engages at setWorkers>1, so a
 * clean in-process stop() is not available (issue #11) — SIGKILL after
 * the client finishes, %A swallows the abrupt exit. Stats are not asserted:
 * in the split the listeners live on thread-clean reactor contexts without
 * the worker's request counters, so getHttp3Stats() does not reflect them. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-037';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    /* The body is computed on the worker from the handed-off request, so
     * a correct echo proves method+uri crossed reactor->worker; the custom
     * 201 + header prove status+headers crossed worker->reactor. */
    $res->setStatusCode(201)
        ->setHeader('content-type', 'text/plain; charset=utf-8')
        ->setBody('echo:' . $req->getMethod() . ':' . $req->getUri());
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin) {
    /* Reactors + workers need a moment to thread up and bind. */
    usleep(600000);

    $cmd = sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d /world GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    $status = null;
    if (preg_match('/^STATUS=(\d+)$/m', $out, $m)) $status = (int)$m[1];
    $body = preg_replace('/^STATUS=\d+\n?/m', '', $out);

    echo "status=", $status ?? -1, "\n";
    echo "body=",   trim($body), "\n";

    /* The reactor-owned listeners must surface through getHttp3Stats() so a
     * pooled server is observable (#80). Aggregate across listener entries —
     * SO_REUSEPORT spreads the single connection across one reactor. */
    $req_recv = 0; $resp_sub = 0;
    foreach ($server->getHttp3Stats() as $st) {
        $req_recv += (int)($st['h3_request_received']   ?? 0);
        $resp_sub += (int)($st['h3_response_submitted'] ?? 0);
    }
    echo "stats_request_received_ge1=",   ($req_recv >= 1 ? 1 : 0), "\n";
    echo "stats_response_submitted_ge1=", ($resp_sub >= 1 ? 1 : 0), "\n";

    /* Issue #11: no clean cross-thread shutdown for the pool yet; SIGKILL
     * skips PHP shutdown so the worker threads cannot deadlock on exit. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
%Astatus=201
body=echo:GET:/world
stats_request_received_ge1=1
stats_response_submitted_ge1=1
%A
