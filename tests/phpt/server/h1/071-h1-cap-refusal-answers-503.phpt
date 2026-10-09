--TEST--
HTTP/1: a connection refused at the connection cap on a plaintext listener gets a 503 before the close
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_fault_enable')) die('skip needs --enable-fault-injection');
?>
--FILE--
<?php
/* The plaintext twin of tls/022: the same fault point puts the accept on the
 * hard-cap branch, and a plaintext listener answers with the 503 the TLS one
 * withholds. */
require_once __DIR__ . '/../_free_port.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$point = 'server/accept/at_cap';
$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
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

echo "hits: ", _http_fault_hits($point), "\n";
echo "bytes before the close: ", strlen($got), $got === '' ? '' : " (" . strtok($got, "\r\n") . ")", "\n";
echo "refused at cap: ", var_export($refused, true), "\n";
--EXPECT--
hits: 1
bytes before the close: 94 (HTTP/1.1 503 Service Unavailable)
refused at cap: 1
