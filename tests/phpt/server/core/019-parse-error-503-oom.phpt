--TEST--
HttpServer: body preallocation OOM returns 503 + server stays alive (Fix A)
--EXTENSIONS--
true_async_server
true_async
--INI--
memory_limit=32M
--FILE--
<?php
/* Reproducer for PLAN_HTTP2 Step 10 follow-up #1 (Fix A).
 *
 * memory_limit is 32 MiB but setMaxBodySize is 128 MiB, and we declare
 * a Content-Length of 100 MiB. zend_string_alloc(100 MiB) in
 * on_headers_complete exceeds Zend MM's budget and triggers a PHP
 * OOM bailout. The fix wraps that alloc in zend_try/zend_catch so the
 * longjmp is caught inside the reactor callback; the parser sets
 * HTTP_PARSE_ERR_OUT_OF_MEMORY, the connection layer emits a 503,
 * and — crucially — the scheduler survives to handle the NEXT request
 * on a fresh connection. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19900 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxBodySize(128 * 1024 * 1024)  /* allow big bodies in principle */
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

// If we ever reach here the fix failed — OOM should fire first.
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('should-not-run')->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    // --- Pass 1: provoke the OOM path. ---
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) { echo "connect failed: $errstr\n"; $server->stop(); return; }

    // Declare a 100 MiB body. The server never even reads it — headers
    // alone trigger zend_string_alloc(100 MiB) which OOMs at 32 MiB.
    $req = "POST / HTTP/1.1\r\n"
         . "Host: x\r\n"
         . "Content-Length: 104857600\r\n"
         . "\r\n";
    fwrite($fp, $req);

    stream_set_timeout($fp, 3);
    $response = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $response .= $c;
    }
    fclose($fp);

    $line = explode("\r\n", $response)[0] ?? '';
    echo "oom_pass_status: $line\n";

    // --- Pass 2: the survive check. Open a NEW connection and make a
    // trivial request. If the OOM in pass 1 crashed the scheduler, this
    // either won't connect or will read nothing. ---
    $fp2 = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp2) { echo "survive connect failed: $errstr\n"; $server->stop(); return; }
    fwrite($fp2, "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    stream_set_timeout($fp2, 3);
    $response2 = '';
    while (!feof($fp2)) {
        $c = fread($fp2, 8192);
        if ($c === '' || $c === false) break;
        $response2 .= $c;
    }
    fclose($fp2);
    $line2 = explode("\r\n", $response2)[0] ?? '';
    echo "survive_pass_status: $line2\n";

    $tel = $server->getTelemetry();
    echo "parse_errors_4xx_total=" . $tel['parse_errors_4xx_total'] . "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECTF--
Fatal error: Allowed memory size of %d bytes exhausted%a
oom_pass_status: HTTP/1.1 503 Service Unavailable
survive_pass_status: HTTP/1.1 200%a
parse_errors_4xx_total=1
done
