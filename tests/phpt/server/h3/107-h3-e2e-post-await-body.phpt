--TEST--
HttpServer: HTTP/3 POST end-to-end — handler awaits body, echoes length+sha1
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
/* Step 5a regression — handler dispatched at END_HEADERS (before body
 * arrives), then suspends inside $request->awaitBody(). The terminal
 * STREAM-FIN finalizes the body and triggers body_event; the handler
 * resumes and echoes back len+sha1 so a wrong wake-up surfaces as
 * either a hang or a hash mismatch. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-107';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$body_path = $tmp . '/body.bin';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

/* Deterministic 64 KiB so PHP and handler can compare hashes directly. */
$body = str_repeat("0123456789abcdef", 4096);
file_put_contents($body_path, $body);
$expected = sprintf("len=%d sha1=%s method=POST",
    strlen($body), sha1($body));

$port = 20400 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    /* Explicit awaitBody — proves the handler ran BEFORE the full
     * body landed (otherwise this is a no-op; the auto-await path
     * already buffers). Wake comes from body_event in
     * http3_finalize_request_body. */
    $req->awaitBody();
    $b = $req->getBody();
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain')
        ->setBody(sprintf("len=%d sha1=%s method=%s",
            strlen($b), sha1($b), $req->getMethod()));
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin, $body_path, $expected) {
    usleep(80000);
    $cmd = sprintf('%s 127.0.0.1 %d /upload POST %s 2>&1',
        escapeshellarg($client_bin), $port, escapeshellarg($body_path));
    $out = shell_exec($cmd) ?? '';

    $status = null;
    if (preg_match('/^STATUS=(\d+)$/m', $out, $m)) $status = (int)$m[1];
    $body = trim(preg_replace('/^STATUS=\d+\n?/m', '', $out));

    echo "status=", $status ?? -1, "\n";
    echo "match=", ($body === $expected ? "yes" : "no\n  got=$body\n  want=$expected"), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @unlink($body_path); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
status=200
match=yes
done
