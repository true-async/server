<?php
/*
 * Static-file bench server. Mounts StaticHandler at /static/ over a
 * tmp docroot containing fixed-size sample files; the handler serves
 * everything from C without entering the PHP VM.
 *
 * Layout under $root:
 *   tiny.txt   -- 256 B   (one-shot inline path)
 *   small.html -- 16 KiB  (single sendmsg)
 *   medium.bin -- 256 KiB
 *   large.bin  -- 8 MiB   (async file delivery / sendfile)
 *
 * A trivial fallback PHP handler is registered so misrouted requests
 * are visible as 200 from PHP rather than 404 from C.
 *
 * Args:
 *   $argv[1] -- HTTP/1.1 port (default 18090)
 *   $argv[2] -- HTTP/2 port   (default 18091, h2c prior-knowledge)
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;

$port  = (int)($argv[1] ?? 18090);
$h2    = (int)($argv[2] ?? 18091);
$root  = sys_get_temp_dir() . '/bench_static_root';
@mkdir($root, 0700, true);

$write = function (string $name, int $bytes) use ($root): void {
    $path = $root . '/' . $name;
    if (is_file($path) && filesize($path) === $bytes) {
        return;
    }
    $fh = fopen($path, 'wb');
    /* Fill with a non-trivial repeating pattern so any compressors
     * do not collapse the body into nothing. */
    $chunk = str_repeat("0123456789abcdef\n", 1024);
    $clen  = strlen($chunk);
    $left  = $bytes;
    while ($left > 0) {
        $w = $left < $clen ? substr($chunk, 0, $left) : $chunk;
        fwrite($fh, $w);
        $left -= strlen($w);
    }
    fclose($fh);
};

$write('tiny.txt',    256);
$write('small.html',  16   * 1024);
$write('medium.bin',  256  * 1024);
$write('large.bin',   8    * 1024 * 1024);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->addHttp2Listener('127.0.0.1', $h2)
    ->setBacklog(512)
    ->setReadTimeout(15)
    ->setWriteTimeout(30);

$server = new HttpServer($config);

$static = (new StaticHandler('/static/', $root))
    ->setEtagEnabled(true)
    ->setCacheControl('public, max-age=60')
    ->setOpenFileCache(1024, 60);
$server->addStaticHandler($static);

$server->addHttpHandler(function ($request, $response) {
    $response->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody("OK\n");
});

fprintf(
    STDERR,
    "static bench server listening on 127.0.0.1:%d (h1) / %d (h2c) root=%s pid=%d\n",
    $port, $h2, $root, getmypid()
);
$server->start();
