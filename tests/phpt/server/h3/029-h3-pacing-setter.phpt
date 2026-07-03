--TEST--
HttpServerConfig: setHttp3Pacing — opt-in QUIC pacing, default off, delivers correctly when on (#59)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* Pacing (#59 Phase 2) is opt-in: off by default (a lossless path gains
 * nothing and pays for it), on for constrained-path deployments. This
 * verifies the setter/getter + locked guard, and that with pacing ON a
 * multi-record body still arrives byte-exact (pacing must not break or
 * stall delivery — on loopback the sub-threshold gap drains inline). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

/* Part A: default + setter + getter. */
$c = new HttpServerConfig();
echo "default=" . ($c->isHttp3Pacing() ? "on" : "off") . "\n";   /* off */
$c->setHttp3Pacing(true);
echo "after-on=" . ($c->isHttp3Pacing() ? "on" : "off") . "\n";  /* on */
$c->setHttp3Pacing(false);
echo "after-off=" . ($c->isHttp3Pacing() ? "on" : "off") . "\n"; /* off */

$tmp = __DIR__ . '/tmp-029';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$size = 256 * 1024;
$body = str_repeat("\0", $size);
for ($i = 0; $i < $size; $i++) { $body[$i] = chr(($i * 31 + 7) & 0xff); }
$want_sha = sha1($body);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setHttp3Pacing(true);                  /* pacing ON for this server */
$server = new HttpServer($config);

/* Part B: locked guard — config frozen once handed to the server. */
try { $config->setHttp3Pacing(false); echo "locked=no\n"; }
catch (Throwable $e) { echo "locked=yes\n"; }

$server->addHttpHandler(function ($req, $res) use ($body) { $res->setBody($body); });

$client_bin = __DIR__ . '/../../../h3client/h3client';
$client = spawn(function () use ($server, $port, $client_bin, $size, $want_sha) {
    usleep(120000);
    $b = shell_exec(sprintf('%s 127.0.0.1 %d / GET 2>/dev/null',
        escapeshellarg($client_bin), $port)) ?? '';
    printf("paced-body: len=%d match=%d\n", strlen($b),
        (strlen($b) === $size && sha1($b) === $want_sha) ? 1 : 0);
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
default=off
after-on=on
after-off=off
locked=yes
paced-body: len=262144 match=1
done
