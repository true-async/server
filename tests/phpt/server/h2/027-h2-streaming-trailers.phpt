--TEST--
HttpServer: HTTP/2 trailers on the STREAMING response path (gRPC unlock)
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
/* Trailers on the send()/end() STREAMING path — distinct from 003 which
 * uses the buffered setBody() commit. Before the fix, h2_stream_mark_ended
 * resumed + EOF'd the stream but never submitted trailers, so grpc-status
 * after a server-streaming/bidi body was silently dropped. Handler streams
 * two DATA frames, then sets the gRPC trailers; the terminal
 * HEADERS(trailers, END_STREAM) must still go out. */

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
$server->addHttpHandler(function($req, $resp) {
    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'application/grpc');
    /* Streaming send() path: commits HEADERS, then two DATA frames. */
    $resp->send('msg-one');
    $resp->send('msg-two');
    /* Trailers set before the stream ends — carried by the terminal
     * HEADERS(trailers) frame that mark_ended now emits. */
    $resp->setTrailer('grpc-status', '0')
         ->setTrailer('grpc-message', 'ok');
    $resp->end();
});

$client = spawn(function() use ($port, $server) {
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
    echo "saw_ctype=",        (int)(strpos($blob, 'application/grpc') !== false), "\n";
    echo "saw_body=",         (int)(strpos($blob, 'msg-onemsg-two') !== false), "\n";
    echo "saw_grpc_status=",  (int)(strpos($blob, 'grpc-status: 0') !== false), "\n";
    echo "saw_grpc_message=", (int)(strpos($blob, 'grpc-message: ok') !== false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
curl_rc=0
saw_status_200=1
saw_ctype=1
saw_body=1
saw_grpc_status=1
saw_grpc_message=1
Done
