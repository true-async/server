--TEST--
StaticHandler: an unreadable file under on_missing: Next runs the PHP handler once (#348)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (function_exists('posix_geteuid') && posix_geteuid() === 0) die('skip root reads a file of mode 0');
?>
--FILE--
<?php
/* The mount's checks pass for a file that exists, so the send-file engine
 * tries to open it; open() fails on a file of mode 0 (on Windows, on a file
 * whose ACL denies reading its data but not its attributes), and under
 * on_missing: Next the request goes to PHP as for a missing file. It goes
 * there once: one handler call, one answer, on the same connection a second
 * request still works. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/locked.txt", "secret");
if (PHP_OS_FAMILY === 'Windows') {
    /* chmod 0 only sets the read-only attribute there. Everyone (S-1-1-0)
     * loses read-data (RD); stat still works. */
    $acl_path = str_replace('/', '\\', "$root/locked.txt");
    exec(sprintf('icacls %s /deny *S-1-1-0:(RD) 2>&1', escapeshellarg($acl_path)), $out, $rc);
    if ($rc !== 0) {
        echo "icacls failed: ", implode("\n", $out), "\n";
    }
} else {
    chmod("$root/locked.txt", 0);
}

register_shutdown_function(function () use ($root) {
    if (PHP_OS_FAMILY === 'Windows') {
        exec(sprintf('icacls %s /remove:d *S-1-1-0 2>&1',
            escapeshellarg(str_replace('/', '\\', "$root/locked.txt"))));
    }
    @chmod("$root/locked.txt", 0600);
    @unlink("$root/locked.txt");
    @rmdir($root);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));
$server->addStaticHandler((new StaticHandler('/static/', $root))->setOnMissing(StaticOnMissing::NEXT));

$calls = 0;
$server->addHttpHandler(function ($req, $res) use (&$calls) {
    $calls++;
    $res->setStatusCode(299)->setBody('php')->end();
});

$client = spawn(function () use ($port, $server) {
    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($c, 3);
    fwrite($c, "GET /static/locked.txt HTTP/1.1\r\nHost: t\r\n\r\n"
        . "GET /static/locked.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");

    $in = '';
    while (!feof($c)) {
        $chunk = fread($c, 65536);
        if ($chunk === false || $chunk === '') {
            break;
        }

        $in .= $chunk;
    }

    fclose($c);
    echo "answers: ", substr_count($in, "HTTP/1.1 299"), "\n";
    $server->stop();
});

$server->start();
await($client);
echo "handler calls: $calls\n";
?>
--EXPECT--
answers: 2
handler calls: 2
