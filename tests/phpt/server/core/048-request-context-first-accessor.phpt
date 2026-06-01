--TEST--
HttpServer: request_context() materialises the context when it is the first accessor in the handler
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 20340 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$results = [];

// request_context() is the FIRST context accessor touched in the handler
// (no preceding current_context()). It must materialise the per-request
// context lazily and return a usable object, not null — otherwise
// request_context()->set() would fatal with "on null".
$server->addHttpHandler(function($request, $response) use (&$results, $server) {
    $rc = Async\request_context();
    $results['is_null']      = ($rc === null);
    $rc->set('request_id', 'R-1');
    $results['set_get']      = $rc->get('request_id');
    // The lazily created context is the same object current_context() sees.
    $results['same_as_cur']  = ($rc === Async\current_context());

    $response->setStatusCode(200)->setBody("ok");
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
    fwrite($fp, "GET /r1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    while (!feof($fp)) {
        fread($fp, 8192);
    }
    fclose($fp);
});

$server->start();
await($client);

printf("request_context() is null on first access: %s\n", $results['is_null'] ? 'yes' : 'no');
printf("set/get round-trips: %s\n", $results['set_get']);
printf("request_context() === current_context(): %s\n", $results['same_as_cur'] ? 'yes' : 'no');
--EXPECT--
request_context() is null on first access: no
set/get round-trips: R-1
request_context() === current_context(): yes
