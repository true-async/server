--TEST--
HttpServer: CODEL_TARGET_MS must be a whole number of milliseconds, or start() refuses it (#395)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The variable overrides setBackpressureTargetMs(). It was read with strtol:
 * "abc" became 0 and turned CoDel off, "50ms" became 50, and an out-of-range
 * value was dropped, all without a word. A sojourn sample is taken per request
 * only while CoDel is on (telemetry and the access log are off here), so
 * sojourn_samples shows which target the server took. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

function config_with(?string $env, int $config_target_ms, int $port): HttpServerConfig
{
    putenv($env === null ? 'CODEL_TARGET_MS' : "CODEL_TARGET_MS=$env");

    return (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setBackpressureTargetMs($config_target_ms)
        ->setReadTimeout(5)
        ->setWriteTimeout(5);
}

/* start() refuses before it binds anything, so no client is needed; a server
 * that starts anyway is stopped, and reads "started". */
function refused(string $env): string
{
    $server = new HttpServer(config_with($env, 50, tas_free_port()));
    $server->addHttpHandler(fn($req, $res) => $res->setBody('ok'));
    spawn(function () use ($server) {
        delay(200);
        $server->stop();
    });

    try {
        $server->start();
        return 'started';
    } catch (Throwable $e) {
        return $e::class . ': ' . $e->getMessage();
    }
}

/* One request, then the CoDel sample count of the server that took it. */
function samples(?string $env, int $config_target_ms): string
{
    $port = tas_free_port();
    $server = new HttpServer(config_with($env, $config_target_ms, $port));
    $server->addHttpHandler(fn($req, $res) => $res->setBody('ok'));

    spawn(function () use ($server, $port) {
        delay(50);
        $c = stream_socket_client("tcp://127.0.0.1:$port");
        fwrite($c, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        stream_get_contents($c);
        fclose($c);
        $server->stop();
    });

    $server->start();

    return 'samples=' . $server->getTelemetry()['sojourn_samples'];
}

foreach (['abc', '50ms', '10001', '-5', ' 50'] as $bad) {
    echo "\"$bad\": ", refused($bad), "\n";
}

echo "\"50\" over config 0: ", samples('50', 0), "\n";
echo "\"0\" over config 50: ", samples('0', 50), "\n";
echo "unset, config 50: ", samples(null, 50), "\n";
echo "empty, config 0: ", samples('', 0), "\n";
?>
--EXPECT--
"abc": TrueAsync\HttpServerInvalidArgumentException: CODEL_TARGET_MS must be a whole number of milliseconds from 0 to 10000, got "abc"
"50ms": TrueAsync\HttpServerInvalidArgumentException: CODEL_TARGET_MS must be a whole number of milliseconds from 0 to 10000, got "50ms"
"10001": TrueAsync\HttpServerInvalidArgumentException: CODEL_TARGET_MS must be a whole number of milliseconds from 0 to 10000, got "10001"
"-5": TrueAsync\HttpServerInvalidArgumentException: CODEL_TARGET_MS must be a whole number of milliseconds from 0 to 10000, got "-5"
" 50": TrueAsync\HttpServerInvalidArgumentException: CODEL_TARGET_MS must be a whole number of milliseconds from 0 to 10000, got " 50"
"50" over config 0: samples=1
"0" over config 50: samples=0
unset, config 50: samples=1
empty, config 0: samples=0
