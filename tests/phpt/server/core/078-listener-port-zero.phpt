--TEST--
HttpServer: a listener bound on port 0 reports the port the kernel gave it
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Picking a free port before binding it leaves a window in which the port
 * belongs to nobody, and a parallel test run walks into it. Port 0 closes the
 * window: the kernel assigns at bind time, and getBoundListeners() is where
 * the assignment becomes readable. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpServerInvalidArgumentException;
use function Async\spawn;
use function Async\await;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', 0)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok')->end();
});

echo 'before start: ', count($server->getBoundListeners()), "\n";

$client = spawn(function () use ($server) {
    for ($i = 0; $i < 100 && !$server->isRunning(); $i++) {
        usleep(20000);
    }

    $bound = $server->getBoundListeners();
    $entry = $bound[0] ?? [];

    echo 'entries: ', count($bound), "\n";
    echo 'type: ', $entry['type'] ?? '<none>', "\n";
    echo 'host: ', $entry['host'] ?? '<none>', "\n";
    echo 'port assigned: ', (($entry['port'] ?? 0) > 0 ? 'yes' : 'no'), "\n";
    echo 'tls: ', ($entry['tls'] ?? true) ? 'yes' : 'no', "\n";

    $port = (int) ($entry['port'] ?? 0);
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);

    if ($fp === false) {
        echo "connect: failed\n";
        $server->stop();
        return;
    }

    fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    stream_set_timeout($fp, 2);
    $raw = '';

    while (!feof($fp)) {
        $chunk = fread($fp, 8192);

        if ($chunk === false || $chunk === '') {
            break;
        }

        $raw .= $chunk;
    }

    fclose($fp);

    echo 'served: ', (str_contains($raw, ' 200 ') && str_ends_with($raw, 'ok') ? 'yes' : 'no'), "\n";
    $server->stop();
});

$server->start();
await($client);

echo 'after stop: ', count($server->getBoundListeners()), "\n";

try {
    (new HttpServerConfig())->addListener('127.0.0.1', -1);
    echo "negative port: accepted\n";
} catch (HttpServerInvalidArgumentException $e) {
    echo "negative port: refused\n";
}

/* Several threads on one port-0 listener: the address is bound once into the
 * shared set and every thread adopts a duplicate, so the whole server answers
 * on one port. A parent holds no listen event of its own, and the set is where
 * its answer lives. */
$pool = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', 0)
        ->setWorkers(2)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
);

$pool->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok')->end();
});

$pool_client = spawn(function () use ($pool) {
    for ($i = 0; $i < 150 && !$pool->isRunning(); $i++) {
        usleep(20000);
    }

    usleep(300000);

    $port = (int) ($pool->getBoundListeners()[0]['port'] ?? 0);
    echo 'pool port assigned: ', ($port > 0 ? 'yes' : 'no'), "\n";

    $fp = $port > 0 ? @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2) : false;

    if ($fp === false) {
        echo "pool served: no\n";
        $pool->stop();
        return;
    }

    fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    stream_set_timeout($fp, 2);
    $raw = '';

    while (!feof($fp)) {
        $chunk = fread($fp, 8192);

        if ($chunk === false || $chunk === '') {
            break;
        }

        $raw .= $chunk;
    }

    fclose($fp);

    echo 'pool served: ', (str_contains($raw, ' 200 ') ? 'yes' : 'no'), "\n";
    $pool->stop();
});

$pool->start();
await($pool_client);

echo "Done\n";
?>
--EXPECTF--
before start: 0
entries: 1
type: tcp
host: 127.0.0.1
port assigned: yes
tls: no
served: yes
after stop: 0
negative port: refused
pool port assigned: yes
pool served: yes
%ADone
