--TEST--
WebSocket topics: MQTT filter semantics — '+' is exactly one level, '#' is the rest (and the parent)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The edge cases of '+' and '#' are where a hand-rolled matcher goes wrong, so
 * they are pinned here rather than assumed: '+' matches exactly ONE level (not
 * one-or-more), and "sport/#" matches "sport" itself, not just its children.
 * A subscriber matched by several of its own filters must still get ONE copy. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
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

/* Each client sends its filter as the first frame, then just listens. The last
 * one to connect does the publishing. */
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    foreach ($ws as $msg) {
        [$verb, $arg] = explode(' ', $msg->data, 2) + [1 => ''];

        switch ($verb) {
            case 'sub':
                foreach (explode(',', $arg) as $filter) {
                    $ws->subscribe($filter);
                }
                $ws->send('ok');
                break;

            case 'pub':
                $ws->publish($arg, 'hit:' . $arg);
                break;

            case 'bad':
                try {
                    $ws->subscribe($arg);
                    $ws->send('accepted');
                } catch (Throwable $e) {
                    $ws->send('rejected');
                }
                break;

            case 'pubbad':
                try {
                    $ws->publish($arg, 'x');
                    $ws->send('accepted');
                } catch (Throwable $e) {
                    $ws->send('rejected');
                }
                break;
        }
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    /* filter under test => [topic that must hit, topic that must miss] */
    $cases = [
        'sport/tennis/player1' => ['sport/tennis/player1', 'sport/tennis/player2'],
        'sport/+/player1'      => ['sport/football/player1', 'sport/a/b/player1'],
        'sport/#'              => ['sport/tennis/player1', 'music/jazz'],
        '#'                    => ['anything/at/all', null],
    ];

    $pub = ws_open($port);

    foreach ($cases as $filter => [$hit, $miss]) {
        $sub = ws_open($port);

        ws_write($sub, 'sub ' . $filter);
        ws_await($sub);   // 'ok' — the subscribe has landed in the tree

        ws_write($pub, 'pub ' . $hit);
        $got = ws_await($sub) === 'hit:' . $hit;

        $leaked = false;
        if ($miss !== null) {
            ws_write($pub, 'pub ' . $miss);
            delay(250);   // nothing must arrive, so this one has to be a wait
            $leaked = ws_read_pending($sub) !== '';
        }

        printf("%-22s hit=%s miss=%s\n", $filter,
            $got ? 'yes' : 'no', $leaked ? 'LEAKED' : 'no');

        fclose($sub);
    }

    /* "sport/#" must match the parent "sport" itself — an MQTT rule that a naive
     * matcher gets wrong. */
    $sub = ws_open($port);
    ws_write($sub, 'sub sport/#');
    ws_await($sub);
    ws_write($pub, 'pub sport');
    echo 'parent matched by #: ', ws_await($sub) === 'hit:sport' ? 'yes' : 'no', "\n";
    fclose($sub);

    /* Overlapping filters on ONE connection: one copy, not two. */
    $sub = ws_open($port);
    ws_write($sub, 'sub a/b,a/#,#');
    ws_await($sub);
    ws_write($pub, 'pub a/b');
    $first = ws_await($sub);
    delay(250);                          // a second copy would have time to show up
    $second = ws_read_pending($sub);
    echo 'overlapping filters deliver once: ',
        $first === 'hit:a/b' && $second === '' ? 'yes' : "no ($first/$second)", "\n";
    fclose($sub);

    /* Malformed filters and wildcard publishes are refused. */
    foreach (['sport/#/x', 'sport+', ''] as $bad) {
        ws_write($pub, 'bad ' . $bad);
        echo 'filter ', var_export($bad, true), ': ', ws_await($pub), "\n";
    }

    ws_write($pub, 'pubbad sport/#');
    echo 'publish to a wildcard: ', ws_await($pub), "\n";

    fclose($pub);
    delay(200);
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
sport/tennis/player1   hit=yes miss=no
sport/+/player1        hit=yes miss=no
sport/#                hit=yes miss=no
#                      hit=yes miss=no
parent matched by #: yes
overlapping filters deliver once: yes
filter 'sport/#/x': rejected
filter 'sport+': rejected
filter '': rejected
publish to a wildcard: rejected
done%A
