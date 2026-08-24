--TEST--
StaticHandler::hide(): a pattern naming no directory covers that file at any depth
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A document root mounted at "/" serves whatever the framework put under it,
 * and hide() is the whole of what keeps sources off the wire. An operator
 * writing hide('*.php') means every PHP file; a pattern anchored at the mount
 * root covers index.php and hands admin/tools.php to the client as its own
 * text, which is a disclosure rather than a 404.
 *
 * Rooted patterns keep the other half of the rule: cache/* names one directory
 * at the root, not a directory of that name anywhere. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$root = sys_get_temp_dir() . '/php-http-023-root-' . getmypid();
@mkdir($root . '/admin', 0700, true);
@mkdir($root . '/cache', 0700, true);
@mkdir($root . '/var/cache', 0700, true);

file_put_contents($root . '/index.php',       '<?php SECRET;');
file_put_contents($root . '/admin/tools.php', '<?php SECRET;');
file_put_contents($root . '/app.svg',         'svg-bytes');
file_put_contents($root . '/cache/x.txt',     'rooted');
file_put_contents($root . '/var/cache/x.txt', 'nested');

register_shutdown_function(function () use ($root) {
    foreach (['/index.php', '/admin/tools.php', '/app.svg', '/cache/x.txt', '/var/cache/x.txt'] as $f) {
        @unlink($root . $f);
    }
    foreach (['/admin', '/cache', '/var/cache', '/var'] as $d) {
        @rmdir($root . $d);
    }
    @rmdir($root);
});

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', $port)->setReadTimeout(5)
);

$mount = new StaticHandler('/', $root);
$mount->setOnMissing(StaticOnMissing::NEXT);
$mount->hide('*.php', 'cache/*');
$server->addStaticHandler($mount);

$server->addHttpHandler(function ($req, $res) {
    $res->end('handler:' . $req->getPath());
});

spawn(function () use ($port, $server) {
    usleep(50000);

    foreach (['/app.svg', '/index.php', '/admin/tools.php', '/cache/x.txt', '/var/cache/x.txt'] as $path) {
        $c = stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 3);
        stream_set_timeout($c, 3);
        fwrite($c, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $raw = stream_get_contents($c);
        fclose($c);

        $body = substr($raw, strpos($raw, "\r\n\r\n") + 4);
        echo str_pad($path, 18), '-> ', trim($body), "\n";
    }

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
/app.svg          -> svg-bytes
/index.php        -> handler:/index.php
/admin/tools.php  -> handler:/admin/tools.php
/cache/x.txt      -> handler:/cache/x.txt
/var/cache/x.txt  -> nested
Done
