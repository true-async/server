--TEST--
HttpServer access log: a file body and a compressed body are logged as the octets that left
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* http.response.body.size is read by bandwidth accounting and per-route
 * dashboards, and two shapes never passed their bytes through the response
 * object: a file past the slurp threshold goes from the descriptor to the
 * socket, and a codec re-sizes every chunk the handler hands over. The first
 * was logged as 0, the second as the plaintext the handler wrote.
 *
 * Each transport already counted what it sent; the count is now reported back,
 * so the record describes the wire rather than the handler. The gzip line is
 * asserted as a range because the encoded size depends on the zlib build. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$dir = __DIR__ . '/tmp-067';
@mkdir($dir, 0700, true);
$file = "$dir/asset.bin";
/* Past SEND_FILE_SLURP_THRESHOLD (64 KiB): a smaller file is slurped into the
 * response body and its size was never in doubt. */
file_put_contents($file, str_repeat('x', 200000));

$log = sys_get_temp_dir() . '/php-http-067-access-' . getmypid() . '.log';
@unlink($log);
$fh = fopen($log, 'w+b');

register_shutdown_function(function () use ($dir, $file) {
    @unlink($file); @rmdir($dir);
});

$plain = str_repeat("compressible payload\n", 200);

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
        ->setLogSinks([
            ['type' => 'stream', 'stream' => $fh, 'format' => 'json',
             'category' => 'access', 'level' => LogSeverity::INFO],
        ])
);

$server->addHttpHandler(function ($req, $res) use ($file, $plain) {
    if (str_starts_with($req->getPath(), '/asset')) {
        $res->sendFile($file);
        return;
    }

    $res->setHeader('Content-Type', 'text/html');
    $res->write($plain);
    $res->end();
});

spawn(function () use ($server, $port) {
    usleep(50000);

    foreach ([
        ['/asset', ''],
        ['/gz',    "Accept-Encoding: gzip\r\n"],
    ] as [$target, $extra]) {
        $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);

        if (!$c) {
            continue;
        }

        stream_set_timeout($c, 3);
        fwrite($c, "GET $target HTTP/1.1\r\nHost: x\r\n{$extra}Connection: close\r\n\r\n");

        while (!feof($c)) {
            if (@fread($c, 8192) === false) {
                break;
            }
        }

        fclose($c);
    }

    /* Same file over HTTP/2, whose pump counts in its own state. */
    shell_exec(sprintf('curl --http2-prior-knowledge -s -o /dev/null --max-time 3 '
        . 'http://127.0.0.1:%d/asset-h2 2>/dev/null', $port));

    usleep(50000);
    $server->stop();
});

$server->start();

fflush($fh);
fclose($fh);
$lines = trim((string) file_get_contents($log));
@unlink($log);

$size = [];
foreach (explode("\n", $lines) as $line) {
    if ($line === '') {
        continue;
    }

    $attrs = json_decode($line, true)['Attributes'] ?? [];
    $size[$attrs['url.path'] ?? '?'] = $attrs['http.response.body.size'] ?? -1;
}

echo 'asset h1: ', $size['/asset'] ?? 'missing', "\n";
echo 'asset h2: ', $size['/asset-h2'] ?? 'missing', "\n";

$gz = $size['/gz'] ?? -1;
echo 'gz encoded: ', ($gz > 0 && $gz < strlen($plain)) ? 'yes' : "no ($gz)", "\n";
echo 'gz is not plaintext: ', ($gz !== strlen($plain)) ? 'yes' : 'no', "\n";
echo "Done\n";
?>
--EXPECT--
asset h1: 200000
asset h2: 200000
gz encoded: yes
gz is not plaintext: yes
Done
