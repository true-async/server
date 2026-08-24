--TEST--
StaticHandler mounts at "/" and hands what it does not hold to the handler (#259)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A framework runner serves one directory as the document root and routes
 * everything else, which is a mount at "/". The constructor refused it — the
 * length bound beside the bracket test read a one-character prefix as
 * malformed, while the message spoke of a rule "/" keeps. The resolver takes
 * the whole path as the relative one there, so the three answers below are the
 * mount's contract: a file off the disk, a miss handed on, and a hidden glob
 * that is neither served nor leaked as source. */

require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use function Async\spawn;
use function Async\delay;

$root = sys_get_temp_dir() . '/tas-static-root-' . getmypid();
@mkdir($root . '/assets', 0700, true);
file_put_contents($root . '/assets/app.svg', "<svg/>\n");
file_put_contents($root . '/index.php', "<?php echo 'source';\n");

register_shutdown_function(static function () use ($root) {
    @unlink($root . '/assets/app.svg');
    @unlink($root . '/index.php');
    @rmdir($root . '/assets');
    @rmdir($root);
});

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', $port)->setReadTimeout(5)
);

$mount = new StaticHandler('/', $root);
$mount->setOnMissing(StaticOnMissing::NEXT)->hide('*.php');
$server->addStaticHandler($mount);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('handler:' . $req->getPath());
});

spawn(function () use ($port, $server) {
    delay(200);

    $get = static function (string $path) use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);

        if ($fp === false) {
            return '';
        }

        stream_set_timeout($fp, 3);
        fwrite($fp, "GET {$path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
        $wire = stream_get_contents($fp);
        fclose($fp);

        $parts = explode("\r\n\r\n", $wire, 2);

        return trim($parts[1] ?? '');
    };

    echo $get('/assets/app.svg'), "\n";
    echo $get('/api/users'), "\n";
    echo $get('/index.php'), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
<svg/>
handler:/api/users
handler:/index.php
