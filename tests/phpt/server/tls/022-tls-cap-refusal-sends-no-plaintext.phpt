--TEST--
TLS: a connection refused at the connection cap on a TLS listener is closed without a plaintext 503
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true]);
if (!function_exists('_http_fault_enable')) die('skip needs --enable-fault-injection');
?>
--FILE--
<?php
/* The fault point puts the accept on the hard-cap branch, which a real server
 * reaches only when a pause races an accept already in flight. A plain TCP
 * client reads what the server sends before the close: on a TLS listener a
 * plaintext HTTP response is bytes a TLS client takes for a broken handshake,
 * so nothing may arrive. */
require_once __DIR__ . '/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$tmp = __DIR__ . '/tmp-tls-022';
if (!is_dir($tmp)) { mkdir($tmp, 0700, true); }
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

$point = 'server/accept/at_cap';
$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addHttpHandler(function ($req, $res) {
    $res->setBody('served');
});

$client = spawn(function () use ($port, $server, $point) {
    delay(80);
    _http_fault_enable($point);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    $got = '';

    while (!feof($fp)) {
        $chunk = fread($fp, 8192);

        if ($chunk === '' || $chunk === false) {
            break;
        }

        $got .= $chunk;
    }

    fclose($fp);
    $refused = $server->getTelemetry()['accepts_refused_at_cap_total'] ?? null;
    $server->stop();

    return [$got, $refused];
});

$server->start();
[$got, $refused] = await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);

echo "hits: ", _http_fault_hits($point), "\n";
echo "bytes before the close: ", strlen($got), $got === '' ? '' : " (" . strtok($got, "\r\n") . ")", "\n";
echo "refused at cap: ", var_export($refused, true), "\n";
--EXPECT--
hits: 1
bytes before the close: 0
refused at cap: 1
