--TEST--
StaticHandler: open-file cache survives precompressed sidecar Content-Type (UAF regression)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Regression: when a precompressed .br/.gz sidecar is served, the
 * StaticHandler synthesizes a transient zend_string for the override
 * Content-Type (so the sidecar response advertises the ORIGINAL
 * file's MIME). Earlier the open-file cache stored that pointer
 * borrowed-as-persistent; the next cache hit dereferenced freed
 * memory, producing alternating success / failure and eventually
 * heap corruption under load. Verify: two requests for the same
 * sidecar through one open_file_cache entry — both must succeed
 * with identical headers + bodies. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-precomp-uaf-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);

/* Sidecar must exceed SLURP_THRESHOLD (64 KiB) so the async
 * send_file engine is used on the cache-hit path — that is the
 * code that consults the cached Content-Type and would have UAF'd
 * before the fix. Smaller sidecars take the synchronous-slurp
 * fallback (different code path, separate correctness issue). */
$body = str_repeat('A', 96 * 1024);
$br   = "\xce\x7f" . str_repeat('B', 96 * 1024);
file_put_contents("$root/a.js",    $body);
file_put_contents("$root/a.js.br", $br);
$br_size = filesize("$root/a.js.br");

register_shutdown_function(function() use ($root) {
    @unlink("$root/a.js");
    @unlink("$root/a.js.br");
    @rmdir($root);
});

$port = 19460 + getmypid() % 9;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler(
    (new StaticHandler('/static/', $root))
        ->enablePrecompressed('br')
        ->setEtagEnabled(true)
        ->setOpenFileCache(64, 60)
);

$client = spawn(function() use ($port, $server, $br, $br_size) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }

    $do = function() use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        stream_set_timeout($fp, 2);
        fwrite($fp,
            "GET /static/a.js HTTP/1.1\r\n" .
            "Host: localhost\r\n" .
            "Accept-Encoding: br\r\n" .
            "Connection: close\r\n\r\n");
        $resp = '';
        while (!feof($fp)) { $resp .= fread($fp, 8192); }
        fclose($fp);
        return $resp;
    };

    $r1 = $do();
    $r2 = $do();

    $extract = function(string $r): array {
        [$hd, $body] = explode("\r\n\r\n", $r, 2) + ['', ''];
        $status = '';
        $ce     = '';
        $ct     = '';
        foreach (explode("\r\n", $hd) as $line) {
            if (preg_match('#^HTTP/[0-9.]+ (\d{3})#', $line, $m))    $status = $m[1];
            if (stripos($line, 'content-encoding:') === 0) $ce = trim(substr($line, 17));
            if (stripos($line, 'content-type:') === 0)     $ct = trim(substr($line, 13));
        }
        return [$status, $ce, $ct, $body];
    };
    [$s1, $ce1, $ct1, $b1] = $extract($r1);
    [$s2, $ce2, $ct2, $b2] = $extract($r2);

    echo "r1 status=$s1 ce=$ce1 ct=$ct1 body_match=" . (strlen($b1) === $br_size && $b1 === $br ? 'ok' : 'bad') . "\n";
    echo "r2 status=$s2 ce=$ce2 ct=$ct2 body_match=" . (strlen($b2) === $br_size && $b2 === $br ? 'ok' : 'bad') . "\n";
    echo "headers_match: " . ($s1 === $s2 && $ce1 === $ce2 && $ct1 === $ct2 ? 'ok' : 'bad') . "\n";

    $server->stop();
});

spawn(function() use ($server) { usleep(3000000); if ($server->isRunning()) $server->stop(); });
$server->start();
await($client);
--EXPECTF--
r1 status=200 ce=br ct=%s body_match=ok
r2 status=200 ce=br ct=%s body_match=ok
headers_match: ok
