--TEST--
HttpResponse — a declared Content-Length reaches the HTTP/2 peer, and a short body resets the stream
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* A stream has no body to measure when its headers go out, so the only count
 * it can state is the one its handler declared — and the server then holds the
 * body to it, which is what makes the number safe to send. RFC 9113 §8.1.1
 * makes a mismatch a malformed message the client detects. An undeclared
 * stream states nothing: the DATA frames carry their own boundaries.
 *
 * curl reads the header off the wire; the raw client reads the reset code,
 * because a stream that stopped short and a stream that was reset are the same
 * length and differ only in the frame that ends them. */

require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(15)
        ->setWriteTimeout(15)
);

$server->addHttpHandler(function ($req, $res) {
    switch ($req->getUri()) {
        case '/declared':
            $res->setHeader('Content-Type', 'text/plain')
                ->setHeader('Content-Length', '9');
            $res->write('alpha');
            $res->end('beta');
            return;

        case '/undeclared':
            $res->setHeader('Content-Type', 'text/plain');
            $res->write('alpha');
            $res->end('beta');
            return;

        case '/short':
            $res->setHeader('Content-Type', 'text/plain')
                ->setHeader('Content-Length', '100');
            $res->write('alpha');
            $res->end();
            return;
    }

    $res->end('fine');
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $curl = static function (string $path) use ($port) {
        $cmd = 'curl -sS -o /dev/null -D - --http2-prior-knowledge '
             . '--max-time 10 ' . escapeshellarg("http://127.0.0.1:$port$path") . ' 2>/dev/null';
        $head = (string) shell_exec($cmd);
        $out  = [];

        foreach (preg_split('/\r?\n/', $head) as $line) {
            if (preg_match('/^([^:]+):\s*(.*)$/', $line, $m)) {
                $out[strtolower(trim($m[1]))] = trim($m[2]);
            }
        }

        return $out;
    };

    $h = $curl('/declared');
    echo "declared cl: ", $h['content-length'] ?? '<absent>', "\n";

    $h = $curl('/undeclared');
    echo "undeclared cl: ", $h['content-length'] ?? '<absent>', "\n";

    try {
        $cli = new H2TestClient('127.0.0.1', $port, 15);

        $sid = $cli->sendRequest('GET', '/declared', "127.0.0.1:$port");
        [$status, $body, , $ended] = $cli->collectResponse($sid, true);
        echo "declared status=$status body=$body ended=", (int) $ended, "\n";

        $sid = $cli->sendRequest('GET', '/short', "127.0.0.1:$port");
        [$status, $body, , $ended] = $cli->collectResponse($sid, true);
        echo "short status=$status body=$body ended=", (int) $ended, "\n";
        echo "short reset=", var_export($cli->lastResetCode(), true), "\n";

        /* The reset was the stream's, not the connection's. */
        $sid = $cli->sendRequest('GET', '/ok', "127.0.0.1:$port");
        [$status, $body, , $ended] = $cli->collectResponse($sid, true);
        echo "next=$status/$body/", (int) $ended, "\n";

        $cli->close();
    } catch (\Throwable $e) {
        echo "ERR: ", $e->getMessage(), "\n";
    }

    $server->stop();
});

$server->start();
?>
--EXPECT--
declared cl: 9
undeclared cl: <absent>
declared status=200 body=alphabeta ended=1
short status=200 body=alpha ended=0
short reset=2
next=200/fine/1
