--TEST--
HttpServer: current_context() is shared with child coroutines of the request
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 20200 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$out = null;

$server->addHttpHandler(function($request, $response) use (&$out, $server) {
    $ctx = Async\current_context();
    $ctx->set('user', 'alice');

    // A child coroutine spawned with Async\spawn() inherits the
    // request scope, so it observes the very same context — it can
    // both read the handler's value and write back into it.
    $child = spawn(function() {
        $c = Async\current_context();
        $seen = $c->get('user');
        $c->set('child_note', 'hello-from-child');
        return $seen;
    });
    $childSaw = await($child);

    $out = [
        'child_saw_user'      => $childSaw,
        'parent_sees_child'   => $ctx->get('child_note'),
        'same_context_object' => ($ctx === Async\current_context()),
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
    fwrite($fp, "GET /ctx HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    while (!feof($fp)) {
        fread($fp, 8192);
    }
    fclose($fp);
});

$server->start();
await($client);

echo "child saw user: {$out['child_saw_user']}\n";
echo "parent sees child write: {$out['parent_sees_child']}\n";
echo "same context object: " . ($out['same_context_object'] ? 'yes' : 'no') . "\n";
--EXPECT--
child saw user: alice
parent sees child write: hello-from-child
same context object: yes
