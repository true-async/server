<?php
/* Static file delivery via StaticHandler.
 *
 * Routes: /static/tiny.txt /static/small.html /static/medium.bin
 *         /static/large.bin /static/huge.bin
 * Files are generated in sys_get_temp_dir() on first run.
 */

require __DIR__ . '/_common.php';

use TrueAsync\HttpServer;
use TrueAsync\StaticHandler;

[$mode, $port] = perf_parse_mode($argv);

$root = sys_get_temp_dir() . '/true_async_perf_static';
@mkdir($root, 0700, true);

$ensure = function (string $name, int $bytes) use ($root): void {
    $path = $root . '/' . $name;
    if (is_file($path) && filesize($path) === $bytes) {
        return;
    }
    $fh = fopen($path, 'wb');
    $block = str_repeat("0123456789abcdef\n", 1024); /* 17 KiB */
    $left  = $bytes;
    while ($left > 0) {
        $w = $left < strlen($block) ? substr($block, 0, $left) : $block;
        fwrite($fh, $w);
        $left -= strlen($w);
    }
    fclose($fh);
};

$ensure('tiny.txt',   256);
$ensure('small.html', 16 * 1024);
$ensure('medium.bin', 256 * 1024);
$ensure('large.bin',  8 * 1024 * 1024);

$server = new HttpServer(perf_make_config($mode, $port));

$server->addStaticHandler(
    (new StaticHandler('/static/', $root))
        ->setEtagEnabled(true)
        ->setCacheControl('public, max-age=60')
        ->setOpenFileCache(1024, 60)
);

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->setBody("not found\n");
});

fprintf(STDERR, "perf:static mode=%s port=%d pid=%d root=%s\n",
    $mode, $port, getmypid(), $root);
$server->start();
