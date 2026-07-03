--TEST--
HttpServer: each request handler runs in its own per-request scope
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$results = [];
$n = 0;

$server->addHttpHandler(function($request, $response) use (&$results, &$n, $server) {
    $n++;

    // current_context() materialises the context bound to the
    // per-request scope. Because every request gets its own scope
    // (child of the server scope), the marker written by an earlier
    // request is NOT reachable here — neither locally nor via the
    // parent-scope walk that has() performs.
    $ctx = Async\current_context();

    $results[] = [
        'n'                  => $n,
        'sees_old_marker'    => $ctx->has('marker'),
        'current_is_request' => ($ctx === Async\request_context()),
    ];

    $ctx->set('marker', "req$n");

    $response->setStatusCode(200)->setBody("ok$n");
    $response->end();

    if ($n >= 2) {
        $server->stop();
    }
});

$client = spawn(function() use ($port) {
    usleep(20000);
    for ($i = 1; $i <= 2; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        if (!$fp) {
            echo "connect $i failed: $errstr\n";
            return;
        }
        fwrite($fp, "GET /r$i HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        while (!feof($fp)) {
            fread($fp, 8192);
        }
        fclose($fp);
    }
});

$server->start();
await($client);

foreach ($results as $r) {
    printf("request %d: sees_old_marker=%s current_is_request=%s\n",
        $r['n'],
        $r['sees_old_marker'] ? 'yes' : 'no',
        $r['current_is_request'] ? 'yes' : 'no');
}
--EXPECT--
request 1: sees_old_marker=no current_is_request=yes
request 2: sees_old_marker=no current_is_request=yes
