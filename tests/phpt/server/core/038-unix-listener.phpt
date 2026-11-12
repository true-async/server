--TEST--
HttpServer: AF_UNIX listener — end-to-end request over a unix socket
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

/* On Windows the listener is a named pipe, which takes only a \\.\pipe\
 * name, and PHP has no unix:// transport there: a child process opens the
 * pipe as a file, since a blocking read in this thread would stall the
 * server. */
$windows = PHP_OS_FAMILY === 'Windows';
$path = $windows
    ? '\\\\.\\pipe\\ta-ux-' . getmypid()
    : sys_get_temp_dir() . '/ta-ux-' . getmypid() . '.sock';
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

$clientCoroutine = spawn(function() use ($path, $windows) {
    usleep(10000);  // let the server bind + listen

    $request = "GET /hello?x=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    if ($windows) {
        $script = sys_get_temp_dir() . '/ta-ux-client-' . getmypid() . '.php';
        file_put_contents($script, '<?php $fp = fopen($argv[1], "r+b");'
            . ' if (!$fp) { echo "Client connect failed\n"; exit; }'
            . ' fwrite($fp, base64_decode($argv[2]));'
            . ' while (!feof($fp)) { $c = fread($fp, 8192); if ($c === false || $c === "") break; echo $c; }');
        $proc = proc_open([PHP_BINARY, '-n', $script, $path, base64_encode($request)],
            [0 => ['file', 'nul', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes);
        $response = (string) stream_get_contents($pipes[1]);
        fclose($pipes[1]);
        proc_close($proc);
        @unlink($script);
    } else {
        $fp = @stream_socket_client("unix://$path", $errno, $errstr, 2);
        if (!$fp) {
            echo "Client connect failed: $errstr\n";
            return;
        }

        fwrite($fp, $request);

        $response = '';
        while (!feof($fp)) {
            $response .= fread($fp, 8192);
        }
        fclose($fp);
    }

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
