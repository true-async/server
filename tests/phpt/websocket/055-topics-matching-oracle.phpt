--TEST--
WebSocket topics: the C matcher agrees with an MQTT reference oracle across a full filter×topic matrix
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* 041 pins a handful of wildcard cases by hand. This one is a differential
 * test: a tiny, obviously-correct MQTT matcher written in PHP is the oracle,
 * and every (filter, topic) pair in the matrix is checked against the real C
 * engine. subscriberCount() drives it because it runs the SAME tree walk as
 * publish() (ws_topic_match, should_deliver off), so it is a faithful, and
 * synchronous, proxy for what a publish would deliver — no timing, no flakes.
 * With a single subscriber holding one filter, the count is 1 exactly when the
 * filter matches the topic. The hard cases the matrix reaches on purpose: '+'
 * is exactly one level (never zero, never many), '+' DOES match an empty level
 * ("a//b"), '#' matches the parent, and a trailing slash is a real empty level. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use function Async\spawn;
use function Async\delay;

/* Reference MQTT filter match. '#' (last level) swallows the rest including
 * none; '+' is exactly one level; literals compare equal. A filter matches only
 * when it and the topic run out together. */
function mqtt_matches(string $filter, string $topic): bool
{
    $f = explode('/', $filter);
    $t = explode('/', $topic);
    $fi = 0;
    $ti = 0;

    while ($fi < count($f)) {
        if ($f[$fi] === '#') {
            return true;
        }
        if ($ti >= count($t)) {
            return false;
        }
        if ($f[$fi] === '+' || $f[$fi] === $t[$ti]) {
            $fi++;
            $ti++;
            continue;
        }
        return false;
    }

    return $ti === count($t);
}

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(1)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws) {
    foreach ($ws as $msg) {
        [$verb, $arg] = explode(' ', $msg->data, 2) + [1 => ''];

        switch ($verb) {
            case 'sub':
                $ws->subscribe($arg);
                $ws->send('ok');
                break;

            case 'unsub':
                $ws->unsubscribe($arg);
                $ws->send('ok');
                break;

            case 'count':
                $ws->send((string) $ws->subscriberCount($arg));
                break;
        }
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    /* Wildcards, empty levels, trailing/leading slashes, depth. */
    $filters = [
        'a', 'a/b', 'a/+', 'a/+/c', '+/b', '+',
        '#', 'a/#', 'a/+/#', 'a//b', 'a/b/+', '+/+',
    ];
    $topics = [
        'a', 'a/b', 'a/b/c', 'a/x', 'a/x/c', 'x/b',
        'a//b', 'a/', '/b', 'a/b/c/d', 'x/y', 'a/b/z',
    ];

    $sub = ws_open($port);   // the single subscriber
    $drv = ws_open($port);   // fires the count queries

    $checked = 0;
    $bad     = [];

    foreach ($filters as $f) {
        ws_write($sub, 'sub ' . $f);
        ws_await($sub);   // 'ok' — the subscription is in the tree

        foreach ($topics as $t) {
            ws_write($drv, 'count ' . $t);
            $got    = (int) ws_await($drv);
            $expect = mqtt_matches($f, $t) ? 1 : 0;
            $checked++;

            if ($got !== $expect) {
                $bad[] = sprintf('%s vs %s: engine=%d oracle=%d', $f, $t, $got, $expect);
            }
        }

        ws_write($sub, 'unsub ' . $f);
        ws_await($sub);   // 'ok' — back to an empty tree for the next filter
    }

    foreach ($bad as $line) {
        echo 'MISMATCH ', $line, "\n";
    }

    printf("differential oracle: %d pairs checked, %d mismatches\n",
        $checked, count($bad));

    fclose($sub);
    fclose($drv);
    delay(200);
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
differential oracle: 144 pairs checked, 0 mismatches
done%A
