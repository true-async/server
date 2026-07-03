--TEST--
StaticHandler: static_zero_coroutine_total counter bumps on hard-zero hit (issue #13)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Telemetry counter for the hard-zero (open → stat → headers →
 * sendfile → close) fast path.  Every successful kick-off bumps it.
 * A request that bypasses hard-zero (e.g. a directory request whose
 * indexes all miss → 404 in C) leaves it untouched. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-cnt-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/a.txt", "AAA");
file_put_contents("$root/b.txt", "BBB");

register_shutdown_function(function() use ($root) {
    @unlink("$root/a.txt");
    @unlink("$root/b.txt");
    @rmdir($root);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler((new StaticHandler('/static/', $root))->disableIndex());

$client = spawn(function() use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }

    $do = function(string $req) use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        stream_set_timeout($fp, 2);
        fwrite($fp, $req);
        $resp = '';
        while (!feof($fp)) {
            $chunk = fread($fp, 4096);
            if ($chunk === '' || $chunk === false) break;
            $resp .= $chunk;
            if (strlen($resp) > 16384) break;
        }
        fclose($fp);
        $head = explode("\r\n", $resp)[0] ?? '';
        return $head;
    };

    /* Three hard-zero hits, one synthetic 404 that never reaches
     * ss_kick_off.  Counter must read 3 at the end. */
    echo $do("GET /static/a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "\n";
    echo $do("GET /static/b.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "\n";
    echo $do("GET /static/a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "\n";
    echo $do("GET /static/missing HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "\n";

    $stats = $server->getTelemetry();
    echo "static_zero_coroutine_total=", $stats['static_zero_coroutine_total'] ?? 'MISSING', "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
HTTP/1.1 200 OK
HTTP/1.1 200 OK
HTTP/1.1 200 OK
HTTP/1.1 404 Not Found
static_zero_coroutine_total=3
done
