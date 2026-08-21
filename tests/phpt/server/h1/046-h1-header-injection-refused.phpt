--TEST--
HttpResponse: a header name or value that would split the response is refused
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The status line was one door onto the header block and #198 closed it. This
 * is the other, and the one production code opens: a handler that puts request
 * data into a header value — a redirect target, a request id echoed back — puts
 * whatever the client sent between `name: ` and CRLF. A CR or an LF in there
 * ends the block, so the bytes behind it are read as further fields and, past a
 * blank line, as a second response the server never sent (CWE-113).
 *
 * Refused rather than cleaned, unlike the reason phrase: setHeader() runs while
 * the handler is still there to be told, and a header that cannot be sent as
 * one field is a bug in the handler rather than a byte to paper over.
 *
 * Every refusal has to leave the response untouched — the point of the throw is
 * that the handler can still answer — so each arm reads the wire afterwards. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(5)
);

$split = "/ok\r\nX-Injected: yes\r\nContent-Length: 0\r\n\r\nHTTP/1.1 200 OK";

$server->addHttpHandler(function ($req, $res) use ($split) {
    $report = static function (callable $fn) {
        try {
            $fn();
            echo "  no exception\n";
        } catch (\Throwable $e) {
            echo "  ", get_class($e), "\n";
        }
    };

    switch ($req->getPath()) {
        case '/value':
            echo "value:\n";
            $report(static fn() => $res->setHeader('Location', $split));
            $report(static fn() => $res->setHeader('X-Nul', "a\x00b"));
            $report(static fn() => $res->addHeader('X-Multi', ['fine', "bad\r\nX-Also: yes"]));
            echo "  stored: ", json_encode($res->getHeader('x-multi')), "\n";
            $res->setBody('answered')->end();
            return;

        case '/name':
            echo "name:\n";
            $report(static fn() => $res->setHeader("X-Bad\r\nX-Injected", 'yes'));
            $report(static fn() => $res->setHeader('X Bad', 'yes'));
            $report(static fn() => $res->setHeader('X-Bad:', 'yes'));
            $res->setBody('answered')->end();
            return;

        case '/redirect':
            echo "redirect:\n";
            $report(static fn() => $res->redirect($split, 302));
            $res->setBody('answered')->end();
            return;
    }

    $res->setBody('root')->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fetch = static function ($port, $path) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
        stream_set_timeout($fp, 3);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $all = '';

        while (!feof($fp)) {
            $chunk = fread($fp, 8192);

            if ($chunk === false || $chunk === '') { break; }

            $all .= $chunk;
        }

        fclose($fp);
        return $all;
    };

    foreach (['/value', '/name', '/redirect'] as $path) {
        $raw = $fetch($port, $path);

        echo "$path status: ", rtrim(strtok($raw, "\r\n")), "\n";
        echo "$path injected header: ",
            preg_match('/^x-(injected|also):/mi', $raw) ? 'present' : '<absent>', "\n";
        echo "$path status lines: ", preg_match_all('#^HTTP/1\.#m', $raw), "\n";
    }

    $server->stop();
});

$server->start();
?>
--EXPECT--
value:
  TrueAsync\HttpServerInvalidArgumentException
  TrueAsync\HttpServerInvalidArgumentException
  TrueAsync\HttpServerInvalidArgumentException
  stored: null
/value status: HTTP/1.1 200 OK
/value injected header: <absent>
/value status lines: 1
name:
  TrueAsync\HttpServerInvalidArgumentException
  TrueAsync\HttpServerInvalidArgumentException
  TrueAsync\HttpServerInvalidArgumentException
/name status: HTTP/1.1 200 OK
/name injected header: <absent>
/name status lines: 1
redirect:
  TrueAsync\HttpServerInvalidArgumentException
/redirect status: HTTP/1.1 200 OK
/redirect injected header: <absent>
/redirect status lines: 1
