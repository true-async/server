--TEST--
HttpResponse — a message that carries no body sends none on HTTP/2 either
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The framing rules that make a 204 with a body unreadable belong to HTTP/1,
 * but neither half of the contract does. write() has one implementation and
 * every transport reaches it, so a streaming call on such a status throws
 * here as well. The buffered body is dropped instead of refused — the status
 * may have been chosen after the body was built — and that drop is taken
 * where each transport reads the body for the wire, so HTTP/2 answers to it
 * too. RFC 9110 §6.4.1 puts 204 and 304 among the statuses defined to carry
 * no content; §9.3.2 says the same of every response to HEAD.
 *
 * Read with this repository's own client rather than curl: curl discards
 * content on a 204 whatever the server sent, so an empty body at that end
 * proves nothing about this one. The frames are counted here, and
 * lastResetCode() is asserted null — an empty body because the peer rejected
 * a malformed message would show up as a reset, not as a drop. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/_h2_client.inc';
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

        case '/notmodified':
            $res->setStatusCode(304)->setBody('stale')->end();
            return;

        case '/head':
            $res->setBody('0123456789')->end();
            return;
    }

    $res->end('fine');
});

spawn(function () use ($port, $server) {
    usleep(50000);

    /* One connection for all four: a stream the peer had to reset would
     * leave the next one unanswered, so the last row also proves the first
     * three were framed as the client could read them. */
    $c = new H2TestClient('127.0.0.1', $port, 5);

    foreach ([
        ['GET',  '/streamed'],
        ['GET',  '/buffered'],
        ['GET',  '/notmodified'],
        ['HEAD', '/head'],
    ] as [$method, $path]) {
        $sid = $c->sendRequest($method, $path, "127.0.0.1:$port");
        [$status, $body] = $c->collectResponse($sid);
        printf("%s %s: status=%d body=%s\n", $method, $path, $status, json_encode($body));
    }

    echo "reset: ", var_export($c->lastResetCode(), true), "\n";
    $c->close();

    $server->stop();
});

$server->start();
?>
--EXPECT--
streamed: TrueAsync\HttpServerRuntimeException
streamed committed: 0
GET /streamed: status=500 body="refused\n"
GET /buffered: status=204 body=""
GET /notmodified: status=304 body=""
HEAD /head: status=200 body=""
reset: NULL
