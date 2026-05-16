<?php
/* Streaming Response — handler emits the body in chunks via write().
 *
 * Routes: /stream/<size>/<chunk>
 *   <size>  total response bytes  (e.g. 256k, 1m, 8m)
 *   <chunk> per-write chunk size  (e.g. 4k, 16k, 64k)
 *
 * Each call writes ceil(size/chunk) chunks, then end(). Exercises the
 * chunk-queue ring + h2 backpressure path (HTTP/2) or chunked encoding
 * (HTTP/1.1).
 */

require __DIR__ . '/_common.php';

use TrueAsync\HttpServer;

[$mode, $port] = perf_parse_mode($argv);

function perf_parse_size(string $s): int
{
    $s = strtolower(trim($s));
    if (preg_match('/^(\d+)([kmg]?)$/', $s, $m)) {
        $mul = ['' => 1, 'k' => 1024, 'm' => 1024 * 1024, 'g' => 1024 * 1024 * 1024];
        return (int)$m[1] * $mul[$m[2]];
    }
    return 0;
}

$server = new HttpServer(perf_make_config($mode, $port));

$server->addHttpHandler(function ($req, $resp) {
    $path = $req->getPath();
    if (!preg_match('#^/stream/([^/]+)/([^/]+)$#', $path, $m)) {
        $resp->setStatusCode(404)->setBody("usage: /stream/<size>/<chunk>\n");
        return;
    }
    $total = perf_parse_size($m[1]);
    $chunk = perf_parse_size($m[2]);
    if ($total <= 0 || $chunk <= 0 || $chunk > $total) {
        $resp->setStatusCode(400)->setBody("bad size/chunk\n");
        return;
    }
    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'application/octet-stream')
         ->send();
    $payload = str_repeat('x', $chunk);
    $left    = $total;
    while ($left > 0) {
        $n = $left < $chunk ? $left : $chunk;
        $resp->write($n === $chunk ? $payload : substr($payload, 0, $n));
        $left -= $n;
    }
    $resp->end();
});

fprintf(STDERR, "perf:stream mode=%s port=%d pid=%d\n", $mode, $port, getmypid());
$server->start();
