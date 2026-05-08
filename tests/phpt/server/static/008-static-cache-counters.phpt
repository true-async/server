--TEST--
StaticHandler: open-file cache hit/miss counters bump correctly (issue #13 §5a)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Open-file cache: every static-handler pre-flight either hits the
 * HashTable (skips realpath) or misses (resolves + insert post-stat).
 * First request to a path always misses (cache cold); subsequent
 * requests inside the TTL window hit. A different path also misses
 * once. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-cache-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/a.txt", "AAA");
file_put_contents("$root/b.txt", "BBB");

register_shutdown_function(function() use ($root) {
    @unlink("$root/a.txt");
    @unlink("$root/b.txt");
    @rmdir($root);
});

$port = 19940 + getmypid() % 9;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler(
    (new StaticHandler('/static/', $root))
        ->disableIndex()
        ->setOpenFileCache(64, 60)   /* opt in — cache is off by default */
);

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

    /* a (miss + insert), a (hit), a (hit), b (miss + insert), b (hit).
     * → 2 misses, 3 hits. */
    echo $do("GET /static/a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "\n";
    echo $do("GET /static/a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "\n";
    echo $do("GET /static/a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "\n";
    echo $do("GET /static/b.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "\n";
    echo $do("GET /static/b.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), "\n";

    $stats = $server->getTelemetry();
    echo "hits=",   $stats['static_cache_hits_total']   ?? 'MISSING', "\n";
    echo "misses=", $stats['static_cache_misses_total'] ?? 'MISSING', "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
HTTP/1.1 200 OK
HTTP/1.1 200 OK
HTTP/1.1 200 OK
HTTP/1.1 200 OK
HTTP/1.1 200 OK
hits=3
misses=2
done
