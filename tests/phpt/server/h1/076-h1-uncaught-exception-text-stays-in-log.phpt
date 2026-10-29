--TEST--
An uncaught exception other than HttpException answers 500 and keeps its text and code to the log (#441)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A handler's exception text can hold SQL, bound values or file paths, and its
 * code can be another service's status (an HTTP client's exception carries the
 * upstream's 404). Only an HttpException is an answer: it picks the status and
 * the text. Anything else is a failure, answered 500 "Internal Server Error",
 * with its class, text and place in a warning.
 *
 * The warning does not reach a user error handler: frameworks install one that
 * throws every warning back, and an exception thrown there would be left
 * pending in the request's teardown. A newline in the text stays on the
 * warning's own line. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$handler_calls = 0;
set_error_handler(function (int $no, string $str) use (&$handler_calls) {
    $handler_calls++;
    throw new ErrorException($str, 0, $no);
});

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())->addListener('127.0.0.1', $port));

$server->addHttpHandler(function ($req, $res) {
    if ($req->getPath() === '/http-exception') {
        throw new HttpException('No such order', 404);
    }

    throw new \RuntimeException("SQLSTATE[42S02]: SELECT * FROM users\nWHERE token = 'secret'", 404);
});

spawn(function () use ($port, $server, &$handler_calls) {
    usleep(50000);

    $fetch = static function ($port, $path) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
        stream_set_timeout($fp, 3);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $all = stream_get_contents($fp);
        fclose($fp);
        $split = strpos($all, "\r\n\r\n");

        return [substr($all, 0, $split), substr($all, $split + 4)];
    };

    [$head, $body] = $fetch($port, '/query');
    echo "other: ", strtok($head, "\r\n"), " ", json_encode($body), "\n";
    echo "secret on the wire: ", str_contains($head . $body, 'secret') ? 'yes' : 'no', "\n";

    [$head, $body] = $fetch($port, '/http-exception');
    echo "HttpException: ", strtok($head, "\r\n"), " ", json_encode($body), "\n";

    [$head] = $fetch($port, '/query');
    echo "after a failure: ", strtok($head, "\r\n"), "\n";
    echo "error handler calls: $handler_calls\n";

    $server->stop();
});

$server->start();
?>
--EXPECTF--
Warning: [true-async-server] uncaught exception in http handler: RuntimeException: SQLSTATE[42S02]: SELECT * FROM users WHERE token = 'secret' (in %s:%d) in Unknown on line 0
other: HTTP/1.1 500 Internal Server Error "Internal Server Error"
secret on the wire: no
HttpException: HTTP/1.1 404 No such order "No such order"

Warning: [true-async-server] uncaught exception in http handler: RuntimeException: SQLSTATE[42S02]: SELECT * FROM users WHERE token = 'secret' (in %s:%d) in Unknown on line 0
after a failure: HTTP/1.1 500 Internal Server Error
error handler calls: 0
