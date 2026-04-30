--TEST--
HttpServer: TLS 1.3 session resumption via NewSessionTicket
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
/* Two handshakes on the same server:
 *   #1 full 1-RTT — server emits NewSessionTicket.
 *   #2 resumed    — client replays the ticket via -sess_in.
 * Telemetry must show tls_resumed_total advance; the server's total
 * handshake count advances by 2 while failures stay at 0. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-067';
if (!is_dir($tmp)) mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem"; $key = "$tmp/key.pem";
$sess = "$tmp/session.pem";
if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed
";
    exit(1);
}

$port = 19800 + getmypid() % 20;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$hits = 0;
$server->addHttpHandler(function ($req, $res) use (&$hits, $server) {
    $hits++;
    $res->setStatusCode(200)->setBody("ok:$hits")->end();
    if ($hits === 2) $server->stop();
});

$client = spawn(function () use ($port, $sess) {
    usleep(80000);

    /* Handshake #1: full, save the ticket. A trailing HTTP/1.1 GET
     * gives the server a reason to flush NewSessionTicket before
     * close. Stdin via tempfile so cmd.exe + bash both work. */
    $req = "GET /first HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    $req1_tmp = tempnam(sys_get_temp_dir(), 'p67rq');
    file_put_contents($req1_tmp, $req);
    $cmd1 = sprintf(
        'openssl s_client -connect 127.0.0.1:%d '
        . '-servername localhost -tls1_3 -sess_out %s -ign_eof '
        . '-quiet <%s 2>%s',
        $port, escapeshellarg($sess), escapeshellarg($req1_tmp), tls_dev_null()
    );
    shell_exec($cmd1);
    @unlink($req1_tmp);

    /* Handshake #2: replay the ticket. -sess_in drives resumption. */
    $req2 = "GET /second HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    $req2_tmp = tempnam(sys_get_temp_dir(), 'p67rq');
    file_put_contents($req2_tmp, $req2);
    $cmd2 = sprintf(
        'openssl s_client -connect 127.0.0.1:%d '
        . '-servername localhost -tls1_3 -sess_in %s -ign_eof '
        . '-quiet <%s 2>%s',
        $port, escapeshellarg($sess), escapeshellarg($req2_tmp), tls_dev_null()
    );
    shell_exec($cmd2);
    @unlink($req2_tmp);
});

spawn(function () use ($server) {
    usleep(5_000_000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
await($client);

$t = $server->getTelemetry();
echo "handshakes: "  . ($t['tls_handshakes_total'] >= 2 ? 'ok' : 'bad(' . $t['tls_handshakes_total'] . ')') . "\n";
echo "resumed: "     . ($t['tls_resumed_total']    >= 1 ? 'ok' : 'bad(' . $t['tls_resumed_total']    . ')') . "\n";
echo "failures: "    . ($t['tls_handshake_failures_total'] === 0 ? 'ok' : 'bad') . "\n";
echo "hits: "        . ($hits >= 2 ? 'ok' : 'bad') . "\n";

@unlink($cert); @unlink($key); @unlink($sess); @rmdir($tmp);
echo "Done\n";
--EXPECT--
handshakes: ok
resumed: ok
failures: ok
hits: ok
Done
