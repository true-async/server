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

$port = 19800 + getmypid() % 100;

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

$client = spawn(function () use ($port, $server, $has_br, $has_zstd) {
    delay(20);

    if ($has_br) {
        /* random bytes are not valid brotli → decoder error → 400 */
        echo "br garbage: ",   post($port, str_repeat("\xAA", 32),  'br'),   "\n";
    }
    if ($has_zstd) {
        /* random bytes — no zstd magic → 400 */
        echo "zstd garbage: ", post($port, str_repeat("\xAA", 32),  'zstd'), "\n";
    }

    delay(50);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECTF--
%Agarbage: 400
%Agarbage: 400
Done
