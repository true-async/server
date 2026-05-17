--TEST--
StaticHandler: body cache returns identical bytes on repeated requests + 304 hot-HIT path
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-body-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);

/* Two payloads inside the 64 KiB slurp threshold to hit prefer_inline. */
$payload_small = str_repeat("ABCDEFGH", 8);              // 64 B
$payload_mid   = random_bytes(16 * 1024);                // 16 KiB binary
file_put_contents("$root/small.txt", $payload_small);
file_put_contents("$root/mid.bin",   $payload_mid);

register_shutdown_function(function () use ($root) {
    @unlink("$root/small.txt");
    @unlink("$root/mid.bin");
    @rmdir($root);
});

$port = 19960 + getmypid() % 9;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler(
    (new StaticHandler('/s/', $root))
        ->disableIndex()
        ->setOpenFileCache(64, 60)
);

$client = spawn(function () use ($port, $server, $payload_small, $payload_mid) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }

    $do = function (string $req) use ($port): array {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        stream_set_timeout($fp, 2);
        fwrite($fp, $req);
        $resp = '';
        while (!feof($fp)) {
            $chunk = fread($fp, 65536);
            if ($chunk === '' || $chunk === false) break;
            $resp .= $chunk;
        }
        fclose($fp);
        [$head, $body] = explode("\r\n\r\n", $resp, 2) + ['', ''];
        $lines = explode("\r\n", $head);
        $status = (int)explode(' ', $lines[0])[1];
        $headers = [];
        foreach (array_slice($lines, 1) as $line) {
            $kv = explode(': ', $line, 2);
            if (count($kv) === 2) $headers[strtolower($kv[0])] = $kv[1];
        }
        return [$status, $headers, $body];
    };

    /* ── 1. Repeated small.txt: bytes must be identical across N reads.
     * First request: cache miss → slurp + body_store.
     * Subsequent: hot-HIT short-circuit reads from cache. */
    $bodies = [];
    for ($i = 0; $i < 5; $i++) {
        [$st, $hd, $bd] =
            $do("GET /s/small.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $bodies[] = $bd;
        if ($st !== 200) {
            echo "small[$i] bad status: $st\n";
        }
    }

    $unique = array_unique($bodies);
    echo "small-rounds: ", count($bodies), " unique: ", count($unique), "\n";
    echo "small-bytes-match: ", ($bodies[0] === $payload_small ? 'yes' : 'no'), "\n";

    /* ── 2. Mid (16 KiB binary): random bytes — exposes any off-by-one
     * in persistent copy. */
    [$st1, $hd1, $bd1] =
        $do("GET /s/mid.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    [$st2, $hd2, $bd2] =
        $do("GET /s/mid.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "mid-status: $st1/$st2\n";
    echo "mid-len: ", strlen($bd1), "/", strlen($bd2), " expect=", strlen($payload_mid), "\n";
    echo "mid-equal: ", ($bd1 === $bd2 ? 'yes' : 'no'), "\n";
    echo "mid-orig: ",  ($bd1 === $payload_mid ? 'yes' : 'no'), "\n";

    /* ── 3. 304 hot-HIT branch: capture ETag, then If-None-Match → must
     * be 304 with no body and correct headers (etag + last-modified)
     * built straight from cv. */
    $etag = $hd1['etag'] ?? null;
    if ($etag === null) {
        echo "no-etag\n";
    } else {
        [$st3, $hd3, $bd3] = $do(
            "GET /s/mid.bin HTTP/1.1\r\nHost: x\r\n" .
            "If-None-Match: $etag\r\nConnection: close\r\n\r\n"
        );
        echo "cond-status: $st3 body-len=", strlen($bd3), "\n";
        echo "cond-etag-match: ", (($hd3['etag'] ?? '') === $etag ? 'yes' : 'no'), "\n";
        echo "cond-has-lm: ", (isset($hd3['last-modified']) ? 'yes' : 'no'), "\n";
    }

    /* ── 4. Re-request mid.bin after 304 — body still served correctly
     * from cache (eviction shouldn't have been triggered). */
    [$st4, $hd4, $bd4] =
        $do("GET /s/mid.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "post-cond-equal: ", ($bd4 === $payload_mid ? 'yes' : 'no'), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
small-rounds: 5 unique: 1
small-bytes-match: yes
mid-status: 200/200
mid-len: 16384/16384 expect=16384
mid-equal: yes
mid-orig: yes
cond-status: 304 body-len=0
cond-etag-match: yes
cond-has-lm: yes
post-cond-equal: yes
done
