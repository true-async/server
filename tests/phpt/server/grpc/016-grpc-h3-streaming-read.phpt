--TEST--
gRPC over HTTP/3: incremental readMessage over a streaming request body (issue #26 policy on H3)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h3/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'aioquic' => true]);
?>
--FILE--
<?php
/* H3 mirror of grpc/013: with setBodyStreamingEnabled(true) the H3 dispatch
 * applies the issue-#26 three-case policy, so readMessage() pops body-stream
 * chunks incrementally instead of waiting for the full body. The three
 * messages total 600 KiB — 2.3x the 256 KiB initial stream window — so the
 * transfer only completes if http_body_stream_pop returns deferred QUIC
 * credit as the handler drains (MAX_STREAM_DATA refills). This pins both
 * halves: the streaming policy on H3 AND pop-side flow-control credit. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require __DIR__ . '/../h3/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-grpc-016';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

function grpc_frame(string $m): string {
    return "\x00" . pack('N', strlen($m)) . $m;
}

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setBodyStreamingEnabled(true)
    ->setReadTimeout(10)->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addGrpcHandler(function ($req, $resp) {
    /* NO awaitBody(): each readMessage() suspends until the next message
     * is reassembled from the chunk queue — client-streaming shape. */
    while (($m = $req->readMessage()) !== null) {
        $resp->writeMessage('len:' . strlen($m));
    }
});

$py = __DIR__ . '/_h3grpc_client.py';

$client = spawn(function () use ($server, $port, $py, $tmp) {
    usleep(200000);

    $sizes = [204800, 204800, 204800];   /* 3 x 200 KiB = 600 KiB total */
    $body  = '';
    foreach ($sizes as $i => $sz) {
        $body .= grpc_frame(str_repeat(chr(ord('a') + $i), $sz));
    }

    $hexfile = $tmp . '/body.hex';
    file_put_contents($hexfile, bin2hex($body));

    $cmd = sprintf("python3 %s 127.0.0.1 %d /svc/Collect application/grpc @%s 2>/dev/null",
        escapeshellarg($py), $port, escapeshellarg($hexfile));
    $out = shell_exec($cmd) ?? '';
    @unlink($hexfile);

    /* Deframe every response message from the BODYHEX line. */
    $replies = [];
    if (preg_match('/^BODYHEX ([0-9a-f]*)$/m', $out, $m)) {
        $buf = hex2bin($m[1]); $off = 0;
        while ($off + 5 <= strlen($buf)) {
            $len = unpack('N', substr($buf, $off + 1, 4))[1];
            $replies[] = substr($buf, $off + 5, $len);
            $off += 5 + $len;
        }
    }

    echo "saw_grpc_status=", (int)(strpos($out, 'HDR grpc-status: 0') !== false), "\n";
    echo "count=", count($replies), "\n";
    echo "replies=", implode(',', $replies), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
saw_grpc_status=1
count=3
replies=len:204800,len:204800,len:204800
Done
