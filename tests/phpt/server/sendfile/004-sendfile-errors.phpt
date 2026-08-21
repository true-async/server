--TEST--
HttpResponse::sendFile() — open failure surfaces 500
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
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    /* The status option is a second door onto response->status_code, and it
     * used to be wider than setStatusCode()'s: an interim status hung the
     * exchange, and a status defined to carry no content framed the file under
     * a header block the client ends at the blank line — it then reads the
     * file's first bytes as the next status line. Both are refused here, where
     * the handler can still answer. */
    foreach ([100, 204, 205, 304, 42] as $bad) {
        try {
            $res->sendFile(__FILE__, new TrueAsync\SendFileOptions(status: $bad));
            echo "status $bad: accepted\n";
        } catch (\Throwable $e) {
            echo "status $bad: ", get_class($e), "\n";
        }
    }

    /* Path is well-formed, but the file doesn't exist → FSM emits 500. */
    $res->sendFile('/nonexistent/path/to/file-' . uniqid());
});

$client = spawn(function() use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    $resp = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $resp .= $c;
    }
    fclose($fp);
    $head_end = strpos($resp, "\r\n\r\n");
    $head = substr($resp, 0, $head_end);
    $lines = explode("\r\n", $head);
    echo "status: ", $lines[0], "\n";
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
--EXPECTF--
status 100: TrueAsync\HttpServerInvalidArgumentException
status 204: TrueAsync\HttpServerInvalidArgumentException
status 205: TrueAsync\HttpServerInvalidArgumentException
status 304: TrueAsync\HttpServerInvalidArgumentException
status 42: TrueAsync\HttpServerInvalidArgumentException
status: HTTP/1.1 500 Internal Server Error
done
