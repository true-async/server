--TEST--
HttpServer: TLS error paths — abrupt mid-handshake close + plain HTTP on TLS port
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-071';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed
";
    exit(1);
}

$port = 19990 + getmypid() % 9;

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(3)
    ->setWriteTimeout(3));

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('OK')->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(80000);

    // CASE 1: abrupt mid-handshake close — connect, send 5 bytes of
    // garbage that look like the start of a TLS record, then close.
    // Server must surface this without hanging.
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    $abrupt_ok = (bool)$fp;
    if ($fp) {
        fwrite($fp, "\x16\x03\x01\x00\x05hello");  // looks like TLS handshake start
        fclose($fp);
    }
    echo "abrupt_connected=" . ($abrupt_ok ? 1 : 0) . "\n";

    usleep(100000);

    // CASE 2: plain HTTP on TLS port — must NOT crash, server should
    // either reject or close.
    $fp2 = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    $plain_ok = (bool)$fp2;
    if ($fp2) {
        fwrite($fp2, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        $resp = '';
        $start = microtime(true);
        while (!feof($fp2) && microtime(true) - $start < 1.5) {
            $r = @fread($fp2, 8192);
            if ($r === false || $r === '') break;
            $resp .= $r;
        }
        fclose($fp2);
        echo "plain_resp_len=" . strlen($resp) . "\n";
    }
    echo "plain_connected=" . ($plain_ok ? 1 : 0) . "\n";

    usleep(100000);

    // CASE 3: After error cases, a real TLS request must still work
    // (server didn't get stuck).
    $cmd = sprintf(
        'echo "" | openssl s_client -connect 127.0.0.1:%d -servername localhost -tls1_3 -brief 2>&1',
        $port
    );
    $out = shell_exec($cmd);
    echo "still_alive=" . (str_contains($out, 'TLSv1.3') ? 1 : 0) . "\n";

    $server->stop();
});

spawn(function () use ($server) {
    usleep(3000000);
    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
--EXPECTF--
abrupt_connected=1
plain_resp_len=%d
plain_connected=1
still_alive=1
