--TEST--
StaticHandler::hide(): a leading separator pins the root, a trailing one names a directory, ** crosses
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The three forms an operator carries over from gitignore. Each of them used to
 * match nothing at all, which reads exactly like "hidden" until the file comes
 * back on the wire — the same disclosure #270 was about, reached by writing a
 * pattern in a shape the matcher did not read.
 *
 * /index.php names the root's own file and leaves its namesakes alone, cache/
 * names a directory wherever it sits, logs/** crosses separators the way logs/*
 * does not, and a pattern opening with a double star reaches the root as well
 * as everything under it. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$root = sys_get_temp_dir() . '/php-http-024-root-' . getmypid();
@mkdir($root . '/sub', 0700, true);
@mkdir($root . '/var/cache', 0700, true);
@mkdir($root . '/logs/deep', 0700, true);

$files = [
    '/index.php'          => 'root-secret',
    '/sub/index.php'      => 'nested-copy',
    '/var/cache/x.txt'    => 'cached',
    '/logs/deep/app.log'  => 'logged',
    '/secret.txt'         => 'root-note',
    '/sub/secret.txt'     => 'nested-note',
    '/app.svg'            => 'svg-bytes',
];

foreach ($files as $path => $body) {
    file_put_contents($root . $path, $body);
}

register_shutdown_function(function () use ($root, $files) {
    foreach (array_keys($files) as $path) {
        @unlink($root . $path);
    }
    foreach (['/sub', '/var/cache', '/var', '/logs/deep', '/logs'] as $dir) {
        @rmdir($root . $dir);
    }
    @rmdir($root);
});

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', $port)->setReadTimeout(5)
);

$mount = new StaticHandler('/', $root);
$mount->setOnMissing(StaticOnMissing::NEXT);
$mount->hide('/index.php', 'cache/', 'logs/**', '**/secret.txt');
$server->addStaticHandler($mount);

$server->addHttpHandler(function ($req, $res) {
    $res->end('handler:' . $req->getPath());
});

spawn(function () use ($port, $server) {
    delay(50);

    foreach (array_keys($files = [
        '/app.svg'           => null,
        '/index.php'         => null,
        '/sub/index.php'     => null,
        '/var/cache/x.txt'   => null,
        '/logs/deep/app.log' => null,
        '/secret.txt'        => null,
        '/sub/secret.txt'    => null,
    ]) as $path) {
        $c = stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 3);
        stream_set_timeout($c, 3);
        fwrite($c, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $raw = stream_get_contents($c);
        fclose($c);

        $body = substr($raw, strpos($raw, "\r\n\r\n") + 4);
        echo str_pad($path, 20), '-> ', trim($body), "\n";
    }

    /* A pattern the matcher cannot read would cover nothing, and nothing is
     * what an operator reads as hidden — hide() refuses it instead. */
    $spare = new StaticHandler('/spare/', sys_get_temp_dir());

    try {
        $spare->hide(str_repeat('a', 512));
        echo "512 bytes: accepted\n";
    } catch (\Throwable $e) {
        echo "512 bytes: ", get_class($e), "\n";
    }

    try {
        $spare->hide(str_repeat('a', 513));
        echo "513 bytes: accepted\n";
    } catch (\Throwable $e) {
        echo "513 bytes: ", get_class($e), ' - ', $e->getMessage(), "\n";
    }

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
/app.svg            -> svg-bytes
/index.php          -> handler:/index.php
/sub/index.php      -> nested-copy
/var/cache/x.txt    -> handler:/var/cache/x.txt
/logs/deep/app.log  -> handler:/logs/deep/app.log
/secret.txt         -> handler:/secret.txt
/sub/secret.txt     -> handler:/sub/secret.txt
512 bytes: accepted
513 bytes: TrueAsync\HttpServerInvalidArgumentException - StaticHandler hide pattern must be at most 512 bytes
Done
