--TEST--
HttpServer: an uncapped listener paused anyway answers every connection that arrived during the pause, after the resume
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_pause_listeners')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* Without a cap or CoDel the server never pauses, so it tells the reactor the
 * listener may accept ahead: on Windows libuv's 32 AcceptEx instead of one
 * (dev/DECISIONS.md 2026-11-12). Paused anyway, the listener must still lose
 * nothing and hand nothing over from inside the resume. 40 clients connect
 * during the pause, past the 32 a Windows listener may hold. Clients see the
 * same in both AcceptEx modes; the mode itself shows only in time. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await_all;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

const CLIENTS = 40;

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setBacklog(128)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$served = 0;
$server->addHttpHandler(function ($req, $res) use (&$served) {
    $served++;
    $res->setStatusCode(200)->setBody('ok');
});

spawn(function () use ($server, $port, &$served) {
    delay(50);
    _http_server_pause_listeners($server, true);

    $clients = [];
    for ($i = 0; $i < CLIENTS; $i++) {
        $clients[] = spawn(function () use ($port) {
            $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
            if ($c === false) {
                return "connect failed: $errstr";
            }

            stream_set_timeout($c, 10);
            fwrite($c, "GET / HTTP/1.0\r\nHost: t\r\n\r\n");
            $in = '';
            while (!feof($c)) {
                $chunk = fread($c, 8192);
                if ($chunk === false || $chunk === '') {
                    break;
                }

                $in .= $chunk;
            }

            fclose($c);
            return strtok($in, "\r\n");
        });
    }

    delay(300);
    echo "served while paused: $served\n";

    _http_server_pause_listeners($server, false);
    echo "served inside the resume: $served\n";

    [$results] = await_all($clients);
    $lines = array_count_values($results);
    ksort($lines);
    foreach ($lines as $line => $n) {
        echo "$line: $n\n";
    }

    echo "served: $served\n";
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECT--
served while paused: 0
served inside the resume: 0
HTTP/1.0 200 OK: 40
served: 40
done
