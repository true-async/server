--TEST--
HttpServer: Alt-Svc header advertises HTTP/3 to H1 clients (Step 7)
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
/* Step 7 — RFC 7838 Alt-Svc advertisement.
 *
 * Server runs both a TLS H1/H2 listener and an H3 listener. A plain
 * HTTPS GET via openssl s_client picks H1, and the response carries
 * the Alt-Svc header pointing at the UDP port. PHP_HTTP3_DISABLE_ALT_SVC
 * env var path is exercised in the second sub-test to confirm the
 * opt-out toggle works. */

use TrueAsync\HttpServer;
require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-114';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

function run_server(int $tcp_port, int $udp_port, string $cert, string $key, bool $disable_alt_svc): string {
    $config = (new HttpServerConfig())
        ->addListener('127.0.0.1', $tcp_port, true /* tls */)
        ->addHttp3Listener('127.0.0.1', $udp_port)
        ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
    /* Drive Alt-Svc emission through the config setter — putenv() is
     * unreliable on Windows where PHP's env table doesn't propagate to
     * proc_open children (the env-var path is still honoured at start()
     * as a legacy override, but tests should prefer the typed knob). */
    if ($disable_alt_svc) {
        $config->setHttp3AltSvcEnabled(false);
    }
    $server = new HttpServer($config);
    $server->addHttpHandler(function ($req, $res) {
        $res->setStatusCode(200)->setBody('hi');
    });

    $captured = '';
    $client = spawn(function () use (&$captured, $server, $tcp_port) {
        \Async\delay(80);
        /* Drive a plain HTTPS HEAD via curl. -k accepts the self-signed
         * cert; --http1.1 forces H1 so we can grep the Alt-Svc line out
         * of the dump-header output. */
        $cmd = sprintf('curl -sk --http1.1 -I https://127.0.0.1:%d/ 2>&1', $tcp_port);
        $captured = shell_exec($cmd) ?? '';
        $server->stop();
    });
    $server->start();
    await($client);
    return $captured;
}

$port_tcp = 20900 + getmypid() % 50;
$port_udp = $port_tcp + 1;

$out = run_server($port_tcp, $port_udp, $cert, $key, /* disable */ false);

$has_alt_svc = (preg_match('/^alt-svc:\s*h3=":' . $port_udp . '"; ma=86400/im', $out) === 1);
echo "alt_svc_present=", $has_alt_svc ? 1 : 0, "\n";

$out2 = run_server($port_tcp + 10, $port_udp + 10, $cert, $key, /* disable */ true);
$has_alt_svc2 = (preg_match('/^alt-svc:/im', $out2) === 1);
echo "alt_svc_disabled=", $has_alt_svc2 ? 0 : 1, "\n";

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
alt_svc_present=1
alt_svc_disabled=1
done
