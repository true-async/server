--TEST--
HttpServer: request_context() is per-request and shared across the request subtree
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 20300 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

// Outside of any request there is no request scope, so request_context()
// yields null. The request scope is established only when the request
// handler coroutine is created.
echo "before start: request_context() is " .
    (Async\request_context() === null ? "null" : "set") . "\n";

$results = [];
$n = 0;

$server->addHttpHandler(function($request, $response) use (&$results, &$n, $server) {
    $n++;

    // Either accessor materialises the context on the request scope;
    // here current_context() happens to be called first.
    $cur    = Async\current_context();
    $reqctx = Async\request_context();
    $reqctx->set('request_id', "R-$n");

    // A child in a NESTED scope (parent = this request scope). Its own
    // current_context() differs, but request_context() still resolves
    // to the per-request context, and a parent-walking find() reaches
    // the request_id set above.
    $scope = \Async\Scope::inherit();
    $child = $scope->spawn(function() {
        $own = Async\current_context();
        $rc  = Async\request_context();
        return [
            'rid_via_request_context' => $rc->get('request_id'),
            'rid_via_parent_walk'     => $own->find('request_id'),
            'own_differs'             => ($own !== $rc),
        ];
    });
    $childData = await($child);

    $results[] = [
        'n'                     => $n,
        'handler_cur_is_reqctx' => ($cur === $reqctx),
        'child'                 => $childData,
    ];

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
    printf("request %d: handler current_context === request_context: %s\n",
        $r['n'], $r['handler_cur_is_reqctx'] ? 'yes' : 'no');
    printf("request %d: child request_context request_id: %s\n",
        $r['n'], $r['child']['rid_via_request_context']);
    printf("request %d: child parent-walk request_id: %s\n",
        $r['n'], $r['child']['rid_via_parent_walk']);
    printf("request %d: child current_context !== request_context: %s\n",
        $r['n'], $r['child']['own_differs'] ? 'yes' : 'no');
}
--EXPECT--
before start: request_context() is null
request 1: handler current_context === request_context: yes
request 1: child request_context request_id: R-1
request 1: child parent-walk request_id: R-1
request 1: child current_context !== request_context: yes
request 2: handler current_context === request_context: yes
request 2: child request_context request_id: R-2
request 2: child parent-walk request_id: R-2
request 2: child current_context !== request_context: yes
