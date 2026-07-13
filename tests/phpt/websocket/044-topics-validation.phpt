--TEST--
WebSocket topics: filter/topic validation — wildcards, empty levels, depth cap, publish must be concrete
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The edge cases are MQTT's, so they are settled rather than invented: '#' only
 * as the last level, a wildcard is a whole level and never part of one, empty
 * levels are legal, and a publish topic may not be a pattern — a message fanned
 * out to `user/+/msg` has no destination to be delivered to. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use TrueAsync\WebSocketException;
use function Async\spawn;
use function Async\delay;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(1)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

$deep    = implode('/', array_fill(0, 128, 'a'));   /* exactly WS_TOPIC_MAX_LEVELS */
$tooDeep = implode('/', array_fill(0, 129, 'a'));

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($deep, $tooDeep) {
    $try = function (callable $fn) use ($ws) {
        try { $fn(); return 'ok'; }
        catch (WebSocketException $e) { return 'rejected'; }
    };

    $out = [];

    /* Accepted filters. */
    $out[] = 'a//b: '        . $try(fn() => $ws->subscribe('a//b'));         // empty level is legal
    $out[] = 'sport/#: '     . $try(fn() => $ws->subscribe('sport/#'));
    $out[] = 'sport/+/x: '   . $try(fn() => $ws->subscribe('sport/+/x'));
    $out[] = '#: '           . $try(fn() => $ws->subscribe('#'));
    $out[] = 'depth 128: '   . $try(fn() => $ws->subscribe($deep));

    /* Rejected filters. */
    $out[] = '#/tail: '      . $try(fn() => $ws->subscribe('#/tail'));       // '#' must be last
    $out[] = 'sport+: '      . $try(fn() => $ws->subscribe('sport+'));       // not a whole level
    $out[] = 'empty: '       . $try(fn() => $ws->subscribe(''));
    $out[] = 'depth 129: '   . $try(fn() => $ws->subscribe($tooDeep));

    /* A publish topic must be concrete. */
    $out[] = 'publish a/b: ' . $try(fn() => $ws->publish('a/b', 'x'));
    $out[] = 'publish +: '   . $try(fn() => $ws->publish('user/+/msg', 'x'));
    $out[] = 'publish #: '   . $try(fn() => $ws->publish('sport/#', 'x'));
    $out[] = 'count +: '     . $try(fn() => $ws->subscriberCount('user/+/msg'));

    /* An accepted filter is a subscription; a rejected one is not. */
    $out[] = 'subscribed: ' . count($ws->getTopics());

    /* One frame per line: the test client only decodes frames shorter than 126
     * bytes, and it keeps the failure readable when a line changes. */
    foreach ($out as $line) {
        $ws->send($line);
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(2000);

    $fp = ws_open($port);
    if ($fp === null) { echo "handshake failed\n"; return; }

    for ($i = 0; $i < 14; $i++) {
        echo ws_read($fp), "\n";
    }

    fclose($fp);
    $server->stop();
});

$server->start();
?>
--EXPECTF--
a//b: ok
sport/#: ok
sport/+/x: ok
#: ok
depth 128: ok
#/tail: rejected
sport+: rejected
empty: rejected
depth 129: rejected
publish a/b: ok
publish +: rejected
publish #: rejected
count +: rejected
subscribed: 5%A
