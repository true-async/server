<?php
/**
 * Topic server under test for the third-party WebSocket client in client.py.
 *
 * Everything else that exercises topics is a phpt driving a hand-rolled RFC 6455
 * client we wrote ourselves — which cannot catch a framing habit we got wrong in
 * BOTH the server and the test client. This one is driven by the Python
 * `websockets` library instead: an independent implementation, with
 * permessage-deflate negotiated by default, so the publishes it receives are
 * compressed frames it decodes with code that has never seen ours.
 *
 * Runs multi-worker on purpose: the point of topics is that a publish crosses
 * workers, and a worker is a thread with its own PHP context.
 *
 * Protocol (one line per frame):
 *   sub <filter>      subscribe; replies "ok <filter>"
 *   pub <topic> <msg> publish to <topic>, excluding the sender
 *   count <topic>     replies "count <n>" (scatter/gather over the workers)
 *
 * Usage:  php server.php [port]
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;

$port = (int) ($argv[1] ?? 9101);

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', $port)
    ->setWorkers(4)
    ->setReadTimeout(30)
    ->setWriteTimeout(30)
    ->setWsPingIntervalMs(0)
    ->setWsPermessageDeflate(true);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    foreach ($ws as $msg) {
        $parts = explode(' ', $msg->data, 3);
        $verb  = $parts[0] ?? '';

        switch ($verb) {
            case 'sub':
                $ws->subscribe($parts[1]);
                $ws->send('ok ' . $parts[1]);
                break;

            case 'pub':
                $ws->publish($parts[1], $parts[2] ?? '');
                break;

            case 'count':
                $ws->send('count ' . $ws->subscriberCount($parts[1]));
                break;
        }
    }
});

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(404)->end();
});

fwrite(STDERR, "topics server listening on $port\n");

$server->start();
