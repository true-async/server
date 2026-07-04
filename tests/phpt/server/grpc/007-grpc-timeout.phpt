--TEST--
gRPC: grpc-timeout header parsed and exposed via getGrpcTimeout()
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* The client sends grpc-timeout: 1500m (1.5s). getGrpcTimeout() must parse
 * the value+unit into fractional seconds; a request without the header
 * returns null. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

function grpc_deframe_first(string $buf): string {
    $len = unpack('N', substr($buf, 1, 4))[1];
    return substr($buf, 5, $len);
}

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addGrpcHandler(function($req, $resp) {
    $req->awaitBody();
    $resp->writeMessage('timeout=' . var_export($req->getGrpcTimeout(), true));
});

function grpc_call(int $port, array $headers): string {
    $frame = "\x00" . pack('N', 3) . 'req';
    $bodyfile = tempnam(sys_get_temp_dir(), 'grpcreq');
    $outfile  = tempnam(sys_get_temp_dir(), 'grpcout');
    file_put_contents($bodyfile, $frame);
    $h = '';
    foreach ($headers as $hv) { $h .= '-H ' . escapeshellarg($hv) . ' '; }
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s --max-time 3 %s --data-binary @%s -o %s http://127.0.0.1:%d/svc/M 2>/dev/null',
        $h, escapeshellarg($bodyfile), escapeshellarg($outfile), $port
    );
    shell_exec($cmd);
    $resp = file_get_contents($outfile);
    @unlink($bodyfile); @unlink($outfile);
    return grpc_deframe_first($resp);
}

$client = spawn(function() use ($port, $server) {
    usleep(30000);
    echo grpc_call($port, ['content-type: application/grpc', 'grpc-timeout: 1500m']), "\n";
    echo grpc_call($port, ['content-type: application/grpc']), "\n";
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
timeout=1.5
timeout=NULL
Done
