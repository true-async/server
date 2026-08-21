--TEST--
HttpResponse — a status that carries no body refuses a streaming call on HTTP/2 too
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
/* The framing rules that make a 204 with a body unreadable belong to HTTP/1,
 * but the refusal does not: write() has one implementation and every transport
 * reaches it, so the contract is the same on HTTP/2. RFC 9113 §8.1.1 says the
 * same thing in its own terms — a response with a 204 status and a DATA frame
 * is a malformed message.
 *
 * A buffered body on such a status is dropped rather than refused, here as on
 * HTTP/1: the status may have been chosen after the body was built, and by the
 * time the frames are assembled there is nobody to tell. */

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
        case '/streamed':
            $res->setStatusCode(204);

            try {
                $res->write('leak');
            } catch (\Throwable $e) {
                echo "streamed: ", get_class($e), "\n";
                echo "streamed committed: ", (int) $res->isHeadersSent(), "\n";
                $res->setStatusCode(500)->setBody("refused\n")->end();
                return;
            }

            echo "streamed: no exception\n";
            $res->end();
            return;

        case '/buffered':
            $res->setStatusCode(204)->setBody('oops')->end();
            return;
    }

    $res->end('fine');
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $curl = static function (string $path) use ($port) {
        $cmd = 'curl -sS -D - --http2-prior-knowledge '
             . '--max-time 10 ' . escapeshellarg("http://127.0.0.1:$port$path") . ' 2>/dev/null';
        $raw   = (string) shell_exec($cmd);
        $split = strpos($raw, "\r\n\r\n");

        return [substr($raw, 0, (int) $split), substr($raw, (int) $split + 4)];
    };

    [$head, $body] = $curl('/streamed');
    echo "streamed status: ", rtrim(strtok($head, "\r\n")), "\n";
    echo "streamed body: ", json_encode($body), "\n";

    [$head, $body] = $curl('/buffered');
    echo "buffered status: ", rtrim(strtok($head, "\r\n")), "\n";
    echo "buffered body: ", json_encode($body), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
streamed: TrueAsync\HttpServerRuntimeException
streamed committed: 0
streamed status: HTTP/2 500
streamed body: "refused\n"
buffered status: HTTP/2 204
buffered body: ""
