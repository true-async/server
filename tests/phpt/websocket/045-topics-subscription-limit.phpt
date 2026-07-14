--TEST--
WebSocket topics: setWsMaxSubscriptions caps a connection, refusing the filter without dropping it
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The cap is OFF by default, and that is deliberate: every self-hosted broker
 * defaults to unlimited (EMQX max_subscriptions, NATS max_subs) because only the
 * application knows how many topics it needs. The knob is for the application
 * that pipes client input into subscribe() — `$ws->subscribe($msg->data)` — and
 * does not want an unbounded client growing the worker's topic tree.
 *
 * Over the cap the FILTER is refused and the connection stays up: that is what
 * EMQX answers with SUBACK 0x97 and NATS with -ERR 'Maximum Subscriptions
 * Exceeded'. Dropping the client is an escalation nobody's first response. */
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

echo 'default cap: ', (new HttpServerConfig())->getWsMaxSubscriptions(), "\n";

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(1)
    ->setWsPingIntervalMs(0)
    ->setWsMaxSubscriptions(3);

echo 'configured cap: ', $config->getWsMaxSubscriptions(), "\n";

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $accepted = 0;
    $refused  = 0;

    for ($i = 0; $i < 5; $i++) {
        try { $ws->subscribe("t/$i"); $accepted++; }
        catch (WebSocketException $e) { $refused++; }
    }

    /* A refused filter is not a subscription, and the socket is still usable —
     * this send is the proof it was not torn down. */
    $ws->send("accepted=$accepted refused=$refused held=" . count($ws->getTopics()));

    /* Re-subscribing to one it already holds is idempotent, so it must not be
     * refused for being over the cap: quota is spent on distinct filters. */
    try { $ws->subscribe('t/0'); $ws->send('resubscribe at cap: ok'); }
    catch (WebSocketException $e) { $ws->send('resubscribe at cap: refused'); }

    /* Dropping one frees room for another. */
    $ws->unsubscribe('t/0');

    try { $ws->subscribe('t/9'); $ws->send('after unsubscribe: ok'); }
    catch (WebSocketException $e) { $ws->send('after unsubscribe: refused'); }

    /* Delivery still works on what it does hold. */
    foreach ($ws as $msg) {
        $ws->publish('t/1', 'hit', false);
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    delay(2000);

    $fp = ws_open($port);
    if ($fp === null) { echo "handshake failed\n"; return; }

    echo ws_await($fp), "\n";
    echo ws_await($fp), "\n";
    echo ws_await($fp), "\n";

    ws_write($fp, 'go');
    echo 'still delivers: ', ws_await($fp) === 'hit' ? 'yes' : 'no', "\n";

    fclose($fp);
    $server->stop();
});

$server->start();
?>
--EXPECTF--
default cap: 0
configured cap: 3
accepted=3 refused=2 held=3
resubscribe at cap: ok
after unsubscribe: ok
still delivers: yes%A
