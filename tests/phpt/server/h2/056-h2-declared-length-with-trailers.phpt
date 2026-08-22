--TEST--
HttpServer: a declared length is dropped when the streamed HTTP/2 response carries trailers
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* One response, framed two ways: the buffered commit refuses a stated count
 * when the response carries trailers — nghttp2 puts END_STREAM on the DATA
 * frame that completes the count — while the streaming commit asked only
 * whether a length was declared.
 *
 * The trailers are set before the first write() on purpose. That is the only
 * shape where the two paths know the same thing: headers leave on the first
 * write, so trailers set after it cannot be refused by anything, and the
 * declared length stays on the wire whatever this rule says. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'application/grpc')
         ->setHeader('Content-Length', '14');

    $resp->setTrailer('grpc-status', '0')
         ->setTrailer('grpc-message', 'ok');

    $resp->write('msg-one');
    $resp->write('msg-two');
    $resp->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -v --max-time 3 http://127.0.0.1:%d/ 2>&1',
        $port
    );
    $out = [];
    exec($cmd, $out, $rc);
    $blob = implode("\n", $out);

    echo "curl_rc=$rc\n";
    echo "saw_status_200=",   (int)(strpos($blob, 'HTTP/2 200') !== false), "\n";
    echo "saw_body=",         (int)(strpos($blob, 'msg-onemsg-two') !== false), "\n";
    echo "no_content_length=", (int)(preg_match('/^< content-length:/mi', $blob) === 0), "\n";
    echo "saw_grpc_status=",  (int)(strpos($blob, 'grpc-status: 0') !== false), "\n";
    echo "saw_grpc_message=", (int)(strpos($blob, 'grpc-message: ok') !== false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
curl_rc=0
saw_status_200=1
saw_body=1
no_content_length=1
saw_grpc_status=1
saw_grpc_message=1
Done
