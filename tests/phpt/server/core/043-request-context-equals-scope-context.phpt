--TEST--
HttpServer: request_context() is identical to the request coroutine's scope context
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

$out = null;

$server->addHttpHandler(function($request, $response) use (&$out, $server) {
    // Context of the Scope the request coroutine belongs to.
    $scopeCtx   = Async\current_context();
    // Context of the request.
    $requestCtx = Async\request_context();

    // request_context() must resolve to the very same Async\Context
    // object as the request coroutine's own scope context — identity,
    // not an equal copy.
    $sameObject = ($scopeCtx === $requestCtx);

    // A write through one must be observable through the other:
    // single backing storage confirms it is one object.
    $scopeCtx->set('probe', 'value-42');
    $visibleViaRequestCtx = $requestCtx->get('probe');

    $out = [
        'same_object'             => $sameObject,
        'visible_via_request_ctx' => $visibleViaRequestCtx,
    ];

    $response->setStatusCode(200)->setBody('ok');
    $response->end();
    $server->stop();
});

$client = spawn(function() use ($port) {
    usleep(20000);
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) {
        echo "connect failed: $errstr\n";
        return;
    }
    fwrite($fp, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    while (!feof($fp)) {
        fread($fp, 8192);
    }
    fclose($fp);
});

$server->start();
await($client);

echo "request_context() === scope context: " . ($out['same_object'] ? 'yes' : 'no') . "\n";
echo "write via scope context visible via request_context(): " . $out['visible_via_request_ctx'] . "\n";
--EXPECT--
request_context() === scope context: yes
write via scope context visible via request_context(): value-42
