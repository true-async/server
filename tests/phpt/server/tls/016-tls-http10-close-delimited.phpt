--TEST--
HttpServer: an HTTP/1.0 stream over TLS is delimited by the close
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true, 'curl' => true]);
?>
--FILE--
<?php
/* The framing decision is one predicate, but the bytes take different routes:
 * on plaintext a frame goes out as reactor-owned slots, on TLS it is copied
 * into the BIO ring, and the branch that writes a chunk with nothing around it
 * was reachable only through a declared length until close-delimited framing
 * arrived. This drives that branch over TLS.
 *
 * curl is the reader because it applies the framing rules itself: given no
 * Content-Length and no chunked coding it reads to the TLS close and reports
 * what it got, so a wrong terminator or a stray size line shows up as a body
 * mismatch rather than as a header the test had to interpret. */

require_once __DIR__ . '/_tls_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

$tmp = __DIR__ . '/tmp-016';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';

if (!tls_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port, true)
        ->enableTls(true)
        ->setCertificate($cert)
        ->setPrivateKey($key)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    /* Over the frame-coalescing bound, so the chunk is written on its own
     * rather than copied into one frame with the header block. That standalone
     * write is the identity branch this test exists for, and a small chunk
     * never reaches it. */
    $res->write(str_repeat('a', 64 * 1024));
    $res->write('beta');
    $res->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $cmd = 'curl -sS -k --http1.0 -D - --max-time 10 '
         . escapeshellarg("https://127.0.0.1:$port/stream") . ' 2>' . tls_dev_null();
    $raw   = (string) shell_exec($cmd);
    /* Windows opens the pipe behind shell_exec in text mode and turns every
     * CRLF into LF, header terminator included, so the split takes both. */
    $parts = preg_split("/\r?\n\r?\n/", $raw, 2);
    $head  = $parts[0] ?? '';
    $body  = $parts[1] ?? '';

    echo "status: ", rtrim(strtok($head, "\r\n")), "\n";
    echo "transfer-encoding: ", preg_match('/^transfer-encoding:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "connection: ", preg_match('/^connection:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "body length: ", strlen($body), "\n";
    echo "body tail: ", json_encode(substr($body, -4)), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
status: HTTP/1.0 200 OK
transfer-encoding: <absent>
connection: close
body length: 65540
body tail: "beta"
