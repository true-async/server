--TEST--
HttpServer: N pipelined GETs in one TLS write, ordered responses
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
/* Mirror of h1/001-pipelining but over TLS. The client pipes N full
 * HTTP/1.1 requests into a single `openssl s_client` invocation —
 * openssl ships them as one TLS write. The server must:
 *   1. decode all N from the TLS stream,
 *   2. answer in order (HTTP/1.1 invariant),
 *   3. preserve URIs (no corruption from the pipelined shift).
 *
 * N values mirror h1/001-pipelining.phpt (2, 5, 20). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-065';
if (!is_dir($tmp)) mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem"; $key = "$tmp/key.pem";
if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed
";
    exit(1);
}

$port = 19860 + getmypid() % 20;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);
$total_seen = 0;
$server->addHttpHandler(function ($req, $res) use (&$total_seen) {
    $total_seen++;
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setHeader('X-Uri', $req->getUri())
        ->setBody("uri=" . $req->getUri() . "\n")
        ->end();
});

$client = spawn(function () use ($port, $server) {
    $results = [];
    foreach ([2, 5, 20] as $N) {
        $req = '';
        for ($i = 1; $i <= $N; $i++) {
            $last = ($i === $N) ? "Connection: close\r\n" : "";
            $req .= "GET /p$N-r$i HTTP/1.1\r\nHost: x\r\n$last\r\n";
        }
        /* -ign_eof keeps s_client reading after stdin EOF so we see
         * the full server response; -quiet silences header noise.
         * Stdin via tempfile redirect — `printf %s | …` is bash-only;
         * `cmd.exe` has no printf and no support for the `() || true`
         * subshell idiom. Tempfile + `<` works on both shells. */
        $req_tmp = tempnam(sys_get_temp_dir(), 'p65rq');
        file_put_contents($req_tmp, $req);
        $script = sprintf(
            'openssl s_client -connect 127.0.0.1:%d '
            . '-servername localhost -quiet -ign_eof <%s 2>%s',
            $port, escapeshellarg($req_tmp), tls_dev_null()
        );
        usleep(50000);
        $data = shell_exec($script);
        @unlink($req_tmp);

        $count = substr_count($data ?? '', 'HTTP/1.1 200');
        preg_match_all('/x-uri:\s*(\S+)/i', $data ?? '', $m);
        $uris = $m[1];
        $expected = [];
        for ($i = 1; $i <= $N; $i++) $expected[] = "/p$N-r$i";

        $ok = ($count === $N && $uris === $expected);
        $results[] = "N=$N: " . ($ok ? 'ok' : "FAIL seen=$count uris=" . implode(',', $uris));
    }
    $server->stop();
    return $results;
});

spawn(function () use ($server) {
    usleep(15_000_000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
$results = await($client);
foreach ($results as $r) echo $r, "\n";
echo "total_seen: $total_seen\n";

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "Done\n";
--EXPECT--
N=2: ok
N=5: ok
N=20: ok
total_seen: 27
Done
