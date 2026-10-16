--TEST--
Rate limit (#414): over TLS the refusal is the same 429 with Retry-After, on a connection that stays open
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../tls/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true, 'curl' => true]);
?>
--FILE--
<?php
/* curl sends four requests over one TLS connection; the third and fourth are
 * refused, and curl reports no new connection for any of them after the
 * first: the refusal kept the connection, and the handler ran twice. */

require_once __DIR__ . '/../tls/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$tmp_dir = __DIR__ . '/tmp-098';
@mkdir($tmp_dir, 0700, true);
$cert = "$tmp_dir/cert.pem";
$key  = "$tmp_dir/key.pem";

if (!tls_gen_cert($key, $cert)) {
    die("cert generation failed\n");
}

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$server->addRateLimit(new RateLimit(2, 60));

$handled = 0;
$server->addHttpHandler(function ($req, $res) use (&$handled) {
    $handled++;
    $res->setBody('ok');
});

$client = spawn(function () use ($port, $server, &$handled) {
    delay(50);
    $url = "https://127.0.0.1:$port/";
    $out = shell_exec(sprintf(
        'curl -sk --http1.1 -m 5 -D - -w "transfer %%{http_code} new_connections=%%{num_connects}\n" %s 2>&1',
        str_repeat("-o /dev/null $url ", 4)));

    foreach (preg_split('/\r?\n/', trim($out)) as $line) {
        if (str_starts_with($line, 'transfer') || stripos($line, 'retry-after') === 0) {
            echo strtolower($line), "\n";
        }
    }

    echo "handled=$handled\n";
    $server->stop();
});

$server->start();
await($client);

@unlink($cert);
@unlink($key);
@rmdir($tmp_dir);
echo "done\n";
--EXPECT--
transfer 200 new_connections=1
transfer 200 new_connections=0
retry-after: 30
transfer 429 new_connections=0
retry-after: 30
transfer 429 new_connections=0
handled=2
done
