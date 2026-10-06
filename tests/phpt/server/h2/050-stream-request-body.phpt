--TEST--
HttpRequest::readBody() — H2 streaming request body (issue #26)
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
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setBodyStreamingEnabled(true)
        ->setMaxBodySize(8 * 1024 * 1024)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $total = 0;
    while (($c = $req->readBody()) !== null) {
        $total += strlen($c);
    }
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody("bytes=$total");
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);
    $body = str_repeat('A', 256 * 1024);  // 256 KiB
    $tmp  = tempnam(sys_get_temp_dir(), 'h2body');
    file_put_contents($tmp, $body);
    $cmd  = sprintf(
        'curl --http2-prior-knowledge -s --max-time 5 --data-binary @%s -H "Expect:" http://127.0.0.1:%d/upload',
        escapeshellarg($tmp), $port
    );
    $resp = shell_exec($cmd);
    @unlink($tmp);
    echo $resp, "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
bytes=262144
done
