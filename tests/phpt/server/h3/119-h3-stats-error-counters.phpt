--TEST--
HttpServer: getHttp3Stats() exposes send-error / errqueue counters (raw-fd path)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true]);
?>
--FILE--
<?php
/* The h3-raw-fd-recvmmsg branch added send-error categorisation +
 * MSG_ERRQUEUE drain. The error paths fire only on rare kernel/NIC
 * conditions that we cannot reproduce in a unit test, but the schema
 * regression — "someone removed the counter from the binding" — is
 * worth catching at this level. */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-119';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert_path = $tmp_dir . '/cert.pem';
$key_path  = $tmp_dir . '/key.pem';
if (!h3_gen_cert($key_path, $cert_path)) { echo "cert gen failed\n"; exit(1); }

$port = 20300 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert_path)->setPrivateKey($key_path);
$server = new HttpServer($config);
$server->addHttpHandler(fn($r, $s) => $s->setStatusCode(200)->setBody('x'));

$probe = spawn(function () use ($server) {
    usleep(50000);
    $stats = $server->getHttp3Stats()[0] ?? [];
    foreach ([
        'quic_send_eagain', 'quic_send_gso_refused',
        'quic_send_emsgsize', 'quic_send_unreach',
        'quic_send_other_error', 'quic_gso_disabled',
        'quic_errqueue_emsgsize', 'quic_errqueue_unreach',
        'quic_errqueue_other',
    ] as $key) {
        echo "$key=", array_key_exists($key, $stats) ? 'yes' : 'no',
             ' val=', (int)($stats[$key] ?? -1), "\n";
    }
    $server->stop();
});

$server->start();
await($probe);

@unlink($cert_path); @unlink($key_path); @rmdir($tmp_dir);
echo "done\n";
?>
--EXPECT--
quic_send_eagain=yes val=0
quic_send_gso_refused=yes val=0
quic_send_emsgsize=yes val=0
quic_send_unreach=yes val=0
quic_send_other_error=yes val=0
quic_gso_disabled=yes val=0
quic_errqueue_emsgsize=yes val=0
quic_errqueue_unreach=yes val=0
quic_errqueue_other=yes val=0
done
