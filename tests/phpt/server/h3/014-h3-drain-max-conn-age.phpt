--TEST--
HttpServer: HTTP/3 proactive drain (max_connection_age) emits GOAWAY (Step 6d)
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
/* Step 6d — proactive drain via setMaxConnectionAgeMs.
 *
 * Min age accepted by the validator is 1000ms; handler sleeps 1100ms,
 * comfortably past the 1s ± 10% drain stamp. By the time the response
 * commits in h3_handler_coroutine_dispose, should_drain_state fires
 * and the H3 commit path invokes nghttp3_conn_shutdown to submit a
 * GOAWAY. h3_goaway_sent_total advances. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-111';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$port = 20600 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setMaxConnectionAgeMs(1000);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    \Async\delay(1200);         /* outlive the 1000ms drain stamp + 10% jitter */
    $res->setStatusCode(200)->setBody('drained');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    \Async\delay(80);
    $cmd = sprintf('%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';
    $status = preg_match('/^STATUS=(\d+)$/m', $out, $m) ? (int)$m[1] : -1;
    echo "status=$status\n";

    /* h3_goaway_sent_total lives on the server-wide telemetry
     * (drain counters are protocol-aggregating). */
    $t = $server->getTelemetry();
    echo "h3_goaway_sent_total=",
        (int)($t['h3_goaway_sent_total'] ?? -1), "\n";
    echo "drained_proactive=",
        (int)($t['connections_drained_proactive_total'] ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
status=200
h3_goaway_sent_total=1
drained_proactive=1
done
