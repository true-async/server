--TEST--
HttpResponse: body API — write/setBody/getBody (append vs replace) + protocol getters
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19220 + getmypid() % 1000;

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5));

// Use server stderr → echo at each handler step (the test process'
// stdout is the phpt EXPECT target, so we serialize via probe lines
// that are emitted at the end after await() to keep ordering deterministic).
$lines = [];
$snap = function (string $tag, $val) use (&$lines) {
    $lines[] = "$tag = " . var_export($val, true);
};

$server->addHttpHandler(function ($req, $res) use ($snap, $server) {
    // Snapshot value test: getBody() must return a deep copy that
    // does NOT change when the body buffer is later mutated by write()
    // or setBody(). Each $b<N> below is checked AFTER all subsequent
    // mutations have happened, so any aliasing surfaces as a wrong
    // value here.
    $b0 = $res->getBody();
    $res->write('hello ');
    $b1 = $res->getBody();
    $res->write('world');
    $b2 = $res->getBody();
    $res->setBody('replaced');
    $b3 = $res->getBody();
    $res->write('+more');
    $b4 = $res->getBody();
    $res->setBody('');
    $b5 = $res->getBody();

    // Each captured value must reflect the moment of capture, not
    // the post-mutation state.
    $snap('b0_initial', $b0);
    $snap('b1_after_hello', $b1);
    $snap('b2_after_world', $b2);
    $snap('b3_after_replaced', $b3);
    $snap('b4_after_plus_more', $b4);
    $snap('b5_after_empty', $b5);

    $snap('proto_name', $res->getProtocolName());
    $snap('proto_version', $res->getProtocolVersion());

    $res->setHeader('Content-Type', 'text/plain')->setBody('final-body');
    $res->end();
    $server->stop();
});

$cli = spawn(function () use ($port) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    $buf = '';
    while (!feof($fp)) $buf .= fread($fp, 8192);
    fclose($fp);
    echo "=== wire ===\n$buf\n";
});

$server->start();
await($cli);

echo "=== probe ===\n";
foreach ($lines as $l) echo "$l\n";
--EXPECTF--
=== wire ===
HTTP/1.1 200 OK
Content-Length: 10
content-type: text/plain

final-body
=== probe ===
b0_initial = ''
b1_after_hello = 'hello '
b2_after_world = 'hello world'
b3_after_replaced = 'replaced'
b4_after_plus_more = 'replaced+more'
b5_after_empty = ''
proto_name = 'HTTP'
proto_version = '%s'
