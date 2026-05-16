--TEST--
StaticHandler: small precompressed sidecar served from open-file cache keeps Content-Encoding + correct Content-Type
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Cache-hit + small file (≤ 64 KiB) bypasses the async send_file
 * engine and falls into the synchronous-slurp fast path. That path
 * MUST honour the same precompressed-sidecar metadata as the engine
 * path:
 *   - Content-Encoding from picked_encoding
 *   - Content-Type from override_ct (MIME of the ORIGINAL file, not
 *     the .br/.gz extension which would map to application/octet-stream)
 *   - Vary: Accept-Encoding so caches don't conflate codings
 *
 * Pre-fix: first request (cache miss → engine path) responds with
 * ce=br + ct=text/javascript; second request (cache hit → sync slurp)
 * silently drops Content-Encoding and serves brotli bytes as
 * application/octet-stream. A real browser interprets the response
 * as identity and renders gibberish. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-precomp-small-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);

$body = str_repeat('A', 4096);
/* Realistic brotli magic so the bytes never accidentally match a
 * MIME-sniff pattern (HtmlElement / image / etc); 64 bytes total. */
$br   = "\xce\x7f" . str_repeat('B', 62);
file_put_contents("$root/a.js",    $body);
file_put_contents("$root/a.js.br", $br);
$br_size = filesize("$root/a.js.br");

register_shutdown_function(function() use ($root) {
    @unlink("$root/a.js");
    @unlink("$root/a.js.br");
    @rmdir($root);
});

$port = 19480 + getmypid() % 9;
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

    $extract = function(string $r): array {
        [$hd, $body] = explode("\r\n\r\n", $r, 2) + ['', ''];
        $status = ''; $ce = ''; $ct = ''; $vary = '';
        foreach (explode("\r\n", $hd) as $line) {
            if (preg_match('#^HTTP/[0-9.]+ (\d{3})#', $line, $m))    $status = $m[1];
            if (stripos($line, 'content-encoding:') === 0) $ce   = trim(substr($line, 17));
            if (stripos($line, 'content-type:')     === 0) $ct   = trim(substr($line, 13));
            if (stripos($line, 'vary:')             === 0) $vary = trim(substr($line, 5));
        }
        return [$status, $ce, $ct, $vary, $body];
    };

    $r1 = $do();  /* cache miss → async send_file engine */
    $r2 = $do();  /* cache hit  → synchronous-slurp fast path */

    [$s1, $ce1, $ct1, $v1, $b1] = $extract($r1);
    [$s2, $ce2, $ct2, $v2, $b2] = $extract($r2);

    echo "r1 status=$s1 ce=$ce1 ct=$ct1 vary=$v1 body_match=" . (strlen($b1) === $br_size && $b1 === $br ? 'ok' : 'bad') . "\n";
    echo "r2 status=$s2 ce=$ce2 ct=$ct2 vary=$v2 body_match=" . (strlen($b2) === $br_size && $b2 === $br ? 'ok' : 'bad') . "\n";
    echo "headers_match: " . ($s1 === $s2 && $ce1 === $ce2 && $ct1 === $ct2 && $v1 === $v2 ? 'ok' : 'bad') . "\n";

    $server->stop();
});

spawn(function() use ($server) { usleep(3000000); if ($server->isRunning()) $server->stop(); });
$server->start();
await($client);
--EXPECTF--
r1 status=200 ce=br ct=%s vary=Accept-Encoding body_match=ok
r2 status=200 ce=br ct=%s vary=Accept-Encoding body_match=ok
headers_match: ok
