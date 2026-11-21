--TEST--
StaticHandler: a directory requested without a trailing slash under on_missing: Next runs the PHP handler
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* "/docs" names a directory of the mount, so it is not a file the mount can
 * serve. Under on_missing: Next the request goes to PHP, as for a missing
 * file; without the slash the mount does not look for an index file. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir("$root/docs", 0700, true);
file_put_contents("$root/docs/openapi.json", '{}');

register_shutdown_function(function () use ($root) {
    @unlink("$root/docs/openapi.json");
    @rmdir("$root/docs");
    @rmdir($root);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));
$server->addStaticHandler((new StaticHandler('/', $root))->setOnMissing(StaticOnMissing::NEXT));

$paths = [];
$server->addHttpHandler(function ($req, $res) use (&$paths) {
    $paths[] = $req->getUri();
    $res->setStatusCode(299)->setBody('php')->end();
});

$client = spawn(function () use ($port, $server) {
    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($c, 3);
    fwrite($c, "GET /docs HTTP/1.1\r\nHost: t\r\n\r\n"
        . "GET /docs/openapi.json HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");

    $in = '';
    while (!feof($c)) {
        $chunk = fread($c, 65536);
        if ($chunk === false || $chunk === '') {
            break;
        }

        $in .= $chunk;
    }

    fclose($c);
    preg_match_all('~HTTP/1\.1 (\d+)~', $in, $m);
    echo "statuses: ", implode(' ', $m[1]), "\n";
    $server->stop();
});

$server->start();
await($client);
echo "handler paths: ", implode(' ', $paths), "\n";
?>
--EXPECT--
statuses: 299 200
handler paths: /docs
