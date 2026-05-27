--TEST--
StaticHandler: SymlinkPolicy::REJECT — engine open(2) uses O_NOFOLLOW so a symlink swapped in after the open-file-cache entry was validated still 404s
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The open-file cache skips the per-request lstat pre-flight on a hit
 * (the entry is trusted within its TTL). If the on-disk file is replaced
 * by a symlink after it was cached, only the send_file engine's
 * O_NOFOLLOW open(2) still rejects it. Verify a REJECT mount 404s such a
 * request instead of following the link to a file outside the docroot.
 *
 * The file is > SEND_FILE_SLURP_THRESHOLD (64 KiB) so the request stays
 * on the async send_file engine — the small-file inline-slurp fast-path
 * would not exercise the engine open(). Separate Connection: close
 * sockets reuse the same worker's open-file cache (see 008). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticSymlinks;
use function Async\spawn;
use function Async\await;

$root   = sys_get_temp_dir() . '/static-nofollow-' . getmypid() . '-' . bin2hex(random_bytes(4));
$secret = sys_get_temp_dir() . '/static-nofollow-secret-' . getmypid() . '-' . bin2hex(random_bytes(4)) . '.txt';
mkdir($root, 0700, true);
file_put_contents("$root/doc.txt", str_repeat('A', 70000));
file_put_contents($secret, 'TOPSECRET' . str_repeat('B', 70000));

register_shutdown_function(function() use ($root, $secret) {
    @unlink("$root/doc.txt");
    @rmdir($root);
    @unlink($secret);
});

$port = 19970 + getmypid() % 9;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler(
    (new StaticHandler('/files/', $root))
        ->disableIndex()
        ->setSymlinkPolicy(StaticSymlinks::REJECT)
        ->setOpenFileCache(16, 60)
);

$client = spawn(function() use ($port, $server, $root, $secret) {
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
            $chunk = fread($fp, 8192);
            if ($chunk === '' || $chunk === false) break;
            $resp .= $chunk;
            if (strlen($resp) > 131072) break;
        }
        fclose($fp);
        $head = explode("\r\n", $resp)[0] ?? '';
        $leaked = strpos($resp, 'TOPSECRET') !== false ? 'LEAKED' : 'no-leak';
        return [$head, $leaked];
    };

    /* Cold request: cache miss, lstat pre-flight passes (regular file),
     * engine serves it and inserts the open-file-cache entry. */
    [$h, $l] = $do("GET /files/doc.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "cold: $h $l\n";

    /* Swap the file for a symlink pointing outside the docroot. */
    unlink("$root/doc.txt");
    symlink($secret, "$root/doc.txt");

    /* Warm request: cache hit — pre-flight is skipped. Only the engine's
     * O_NOFOLLOW open() stands between the client and the secret. */
    [$h, $l] = $do("GET /files/doc.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "warm: $h $l\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
cold: HTTP/1.1 200 OK no-leak
warm: HTTP/1.1 404 Not Found no-leak
done
