--TEST--
HttpServer: stop() releases the port and a later start() rebinds it (#275)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!PHP_ZTS) die('skip ZTS required');
?>
--FILE--
<?php
/*
 * The shared listen set binds lazily and keeps the master socket until the
 * set is cleared. stop() must clear it, or the port stays occupied by a
 * server nobody is using and the next start() on the same object fails.
 *
 * The check is deliberately blunt: bind, serve one request, stop, and do it
 * again on the same object and the same port. Against a set that never
 * releases, the second start() throws "address already in use".
 */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

function serve_once(HttpServer $server, int $port, string $label): void
{
    $client = spawn(function () use ($port, $server, $label) {
        $fp = false;
        for ($i = 0; $i < 100; $i++) {
            $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
            if ($fp) break;
            usleep(20000);
        }

        if (!$fp) {
            echo "$label: no connection\n";
            $server->stop();
            return;
        }

        fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $resp = '';
        while (!feof($fp)) {
            $chunk = fread($fp, 4096);
            if ($chunk === '' || $chunk === false) break;
            $resp .= $chunk;
        }
        fclose($fp);

        $line = strtok($resp, "\r\n");
        echo "$label: $line\n";
        $server->stop();
    });

    $server->start();
    await($client);
}

$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain')->setBody('ok');
});

serve_once($server, $port, 'first');
serve_once($server, $port, 'second');

echo "Done\n";
?>
--EXPECT--
first: HTTP/1.1 200 OK
second: HTTP/1.1 200 OK
Done
