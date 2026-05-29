--TEST--
HttpServer: AF_UNIX listener — end-to-end request over a unix socket
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (stripos(PHP_OS, 'WIN') === 0) die('skip POSIX-only test (AF_UNIX path semantics)');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$path = sys_get_temp_dir() . '/ta-ux-' . getmypid() . '.sock';
@unlink($path);

$config = (new HttpServerConfig())
    ->addUnixListener($path)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

// addUnixListener must surface in the config like any other listener.
$listeners = $config->getListeners();
echo "Configured listener type: {$listeners[0]['type']}\n";
echo "Configured listener path matches: " . ($listeners[0]['path'] === $path ? 'yes' : 'no') . "\n";

$server = new HttpServer($config);

$requestMethod = null;
$requestUri = null;

$server->addHttpHandler(function($request, $response) use (&$requestMethod, &$requestUri, $server) {
    $requestMethod = $request->getMethod();
    $requestUri = $request->getUri();

    $response->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody('Hello over unix socket!');
    $response->end();

    $server->stop();
});

$clientCoroutine = spawn(function() use ($path) {
    usleep(10000);  // let the server bind + listen

    $fp = @stream_socket_client("unix://$path", $errno, $errstr, 2);
    if (!$fp) {
        echo "Client connect failed: $errstr\n";
        return;
    }

    fwrite($fp, "GET /hello?x=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");

    $response = '';
    while (!feof($fp)) {
        $response .= fread($fp, 8192);
    }
    fclose($fp);

    echo "=== Client received ===\n";
    echo preg_replace("/^Date: [^\r\n]*\r?\n/mi", "", $response);
});

$server->start();
await($clientCoroutine);

echo "\n=== Server side ===\n";
echo "Method: $requestMethod\n";
echo "URI: $requestUri\n";

// stop() must unlink the socket file — nothing left behind.
clearstatcache();
echo "Socket file removed after stop: " . (file_exists($path) ? 'no' : 'yes') . "\n";
--CLEAN--
<?php
@unlink(sys_get_temp_dir() . '/ta-ux-' . getmypid() . '.sock');
?>
--EXPECTF--
Configured listener type: unix
Configured listener path matches: yes
=== Client received ===
HTTP/1.1 200 OK
Content-Length: 23
content-type: text/plain
connection: close

Hello over unix socket!
=== Server side ===
Method: GET
URI: /hello?x=1
Socket file removed after stop: yes
