--TEST--
HttpResponse: Connection and Transfer-Encoding are the server's to state
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The framing headers are the server's, and until now the handler's copy of
 * one was treated as bytes rather than as a request. `Connection: close` on an
 * HTTP/1.1 response reached the peer while the server kept the socket, so the
 * next request was answered on a connection the client had retired — and on a
 * 1.0 response the keep-alive echo overwrote it, which is the same mistake in
 * the other direction. `Transfer-Encoding` was dropped whatever it named, so a
 * handler that set `gzip` sent encoded bytes with no coding declared anywhere.
 *
 * A handler's `Connection: close` is obeyed as the request it is: the field is
 * not copied, the connection is closed, and the peer is told by the same branch
 * that tells it about a drain. Every other value is refused, because a handler
 * cannot make the server keep a socket. `Transfer-Encoding` is accepted only
 * where it names the chunked coding the server would apply anyway. */

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

$server->addHttpHandler(function ($req, $res) {
    $report = static function (callable $fn) {
        try {
            $fn();
            echo "  accepted\n";
        } catch (\Throwable $e) {
            echo "  ", get_class($e), "\n";
        }
    };

    switch ($req->getPath()) {
        case '/refused':
            echo "refused:\n";
            $report(static fn() => $res->setHeader('Connection', 'keep-alive'));
            $report(static fn() => $res->setHeader('Transfer-Encoding', 'gzip'));
            $report(static fn() => $res->setHeader('Transfer-Encoding', 'chunked'));
            echo "  stored TE: ", json_encode($res->getHeader('transfer-encoding')), "\n";
            $res->setBody('answered')->end();
            return;

        case '/close':
            $res->setHeader('Connection', 'close')->setBody('bye')->end();
            return;

        case '/closestream':
            $res->setHeader('Connection', 'close');
            $res->write('str');
            $res->end();
            return;
    }

    $res->setBody('root')->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    /* One keep-alive socket per case, and a second request written onto it
     * after the first response: the header alone never proved the socket was
     * retired, which is the defect. */
    $probe = static function ($port, $path) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
        stream_set_timeout($fp, 3);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");

        $first = '';

        while (!feof($fp)) {
            $chunk = fread($fp, 4096);

            if ($chunk === false || $chunk === '') { break; }

            $first .= $chunk;

            if (str_contains($first, "\r\n\r\n") && !str_contains($first, 'chunked')) {
                if (preg_match('/^content-length:\s*(\d+)/mi', $first, $m)) {
                    $end = strpos($first, "\r\n\r\n") + 4 + (int) $m[1];

                    if (strlen($first) >= $end) { break; }
                }
            }

            if (str_contains($first, "0\r\n\r\n")) { break; }
        }

        usleep(100000);
        fwrite($fp, "GET /root HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
        $second = '';

        while (!feof($fp)) {
            $chunk = fread($fp, 4096);

            if ($chunk === false || $chunk === '') { break; }

            $second .= $chunk;
        }

        fclose($fp);
        return [$first, $second];
    };

    [$first] = $probe($port, '/refused');
    echo "/refused status: ", rtrim(strtok($first, "\r\n")), "\n";
    echo "/refused transfer-encoding: ",
        preg_match('/^transfer-encoding:/mi', $first) ? 'present' : '<absent>', "\n";

    foreach (['/close', '/closestream'] as $path) {
        [$first, $second] = $probe($port, $path);

        echo "$path connection headers: ", preg_match_all('/^connection:/mi', $first), "\n";
        echo "$path connection: ",
            preg_match('/^connection:\s*(\S+)/mi', $first, $m) ? $m[1] : '<absent>', "\n";
        echo "$path answered after close: ", ($second === '' ? 0 : 1), "\n";
    }

    $server->stop();
});

$server->start();
?>
--EXPECT--
refused:
  TrueAsync\HttpServerInvalidArgumentException
  TrueAsync\HttpServerInvalidArgumentException
  accepted
  stored TE: null
/refused status: HTTP/1.1 200 OK
/refused transfer-encoding: <absent>
/close connection headers: 1
/close connection: close
/close answered after close: 0
/closestream connection headers: 1
/closestream connection: close
/closestream answered after close: 0
