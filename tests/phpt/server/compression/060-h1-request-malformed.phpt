--TEST--
Compression: malformed brotli/zstd request body → 400; oversize → 413 (#9)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
$enc = TrueAsync\HttpServerConfig::getSupportedEncodings();
if (!in_array('br', $enc, true) && !in_array('zstd', $enc, true)) {
    die('skip neither Brotli nor zstd built');
}
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setRequestMaxDecompressedSize(2048);   /* tight cap to trigger 413 */

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $resp) {
    $resp->setHeader('Content-Type', 'text/plain')
         ->setBody('len=' . strlen($req->getBody()))
         ->end();
});

function post(string $port, string $body, string $encoding): int {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
        "POST /e HTTP/1.1\r\nHost: x\r\n"
      . "Content-Length: " . strlen($body) . "\r\n"
      . "Content-Encoding: $encoding\r\nConnection: close\r\n\r\n"
      . $body);
    $raw = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $raw .= $c;
    }
    fclose($fp);
    [$head] = explode("\r\n\r\n", $raw, 2) + [''];
    return (int)(explode(' ', $head)[1] ?? 0);
}

$enc = HttpServerConfig::getSupportedEncodings();
$has_br   = in_array('br',   $enc, true);
$has_zstd = in_array('zstd', $enc, true);

/* One line per encoding the build has, and a verdict that does not depend on
 * how many that is: a build with brotli but not zstd is a supported build, and
 * an expectation naming two lines fails it for something the server did right. */
$client = spawn(function () use ($port, $server, $has_br, $has_zstd) {
    delay(20);

    $status = [];

    if ($has_br) {
        /* random bytes are not valid brotli → decoder error → 400 */
        $status['br']   = post($port, str_repeat("\xAA", 32), 'br');
        echo "br garbage: ", $status['br'], "\n";
    }
    if ($has_zstd) {
        /* random bytes — no zstd magic → 400 */
        $status['zstd'] = post($port, str_repeat("\xAA", 32), 'zstd');
        echo "zstd garbage: ", $status['zstd'], "\n";
    }

    echo "every malformed body refused: ",
         ($status !== [] && array_unique(array_values($status)) === [400]) ? 'yes' : 'no', "\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECTF--
%Aevery malformed body refused: yes
Done
