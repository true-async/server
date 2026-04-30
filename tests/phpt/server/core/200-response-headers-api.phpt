--TEST--
HttpResponse: header API surface — setHeader/addHeader/has/get/getLine/getHeaders/resetHeaders
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19200 + getmypid() % 1000;

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5));

$probe = [];
$server->addHttpHandler(function ($req, $res) use (&$probe, $server) {
    // setHeader (string)
    $res->setHeader('X-Single', 'first');
    // setHeader replaces
    $res->setHeader('X-Single', 'second');
    // addHeader appends another value
    $res->addHeader('X-Multi', 'a');
    $res->addHeader('X-Multi', 'b');
    $res->addHeader('X-Multi', 'c');
    // setHeader with array
    $res->setHeader('X-Arr', ['x', 'y']);

    // hasHeader case-insensitive
    $probe['has_lower']    = $res->hasHeader('x-single');
    $probe['has_mixed']    = $res->hasHeader('X-Single');
    $probe['has_missing']  = $res->hasHeader('X-Nope');

    // getHeader returns first; case-insensitive
    $probe['get_first']    = $res->getHeader('X-Multi');
    $probe['get_missing']  = $res->getHeader('X-Nope');

    // getHeaderLine concatenates with ", "
    $probe['get_line']     = $res->getHeaderLine('X-Multi');
    $probe['get_line_arr'] = $res->getHeaderLine('X-Arr');

    // getHeaders returns full assoc
    $all = $res->getHeaders();
    $probe['headers_keys'] = array_keys($all);

    // Then test resetHeaders before sending the real response
    $res->setHeader('X-Will-Vanish', 'gone');
    $had_before_reset = $res->hasHeader('X-Will-Vanish');
    $res->resetHeaders();
    $probe['reset_had']  = $had_before_reset;
    $probe['reset_now']  = $res->hasHeader('X-Will-Vanish');
    $probe['reset_all']  = $res->getHeaders();

    // Now build the actual wire response
    $res->setHeader('Content-Type', 'text/plain')
        ->setHeader('X-Final', 'shipped')
        ->setBody('headers-ok');
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
foreach ($probe as $k => $v) {
    echo $k . ' = ' . (is_array($v) ? '[' . implode(',', $v) . ']' : var_export($v, true)) . "\n";
}
--EXPECTF--
=== wire ===
HTTP/1.1 200 OK
Content-Length: 10
content-type: text/plain
x-final: shipped

headers-ok
=== probe ===
has_lower = true
has_mixed = true
has_missing = false
get_first = 'a'
get_missing = NULL
get_line = 'a, b, c'
get_line_arr = 'x, y'
headers_keys = [%s]
reset_had = true
reset_now = false
reset_all = []
