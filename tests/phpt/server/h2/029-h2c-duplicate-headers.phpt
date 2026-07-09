--TEST--
HttpServer: HTTP/2 duplicate headers combine (", " generic, "; " cookie crumbs — RFC 9113 §8.2.3)
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
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function($req, $resp) {
    $h = $req->getHeaders();
    $resp->setStatusCode(200)
         ->setBody(($h['x-multi'] ?? '-') . '|' . ($h['cookie'] ?? '-'));
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    /* Each -H is its own header field on the wire — two x-multi fields and
     * two cookie fields (the HPACK crumb shape from RFC 9113 §8.2.3). */
    $cmd = sprintf(
        'curl --http2-prior-knowledge -sS --max-time 3 '
        . '-H "x-multi: alpha" -H "x-multi: beta" '
        . '-H "cookie: a=1" -H "cookie: b=2" '
        . 'http://127.0.0.1:%d/',
        $port
    );
    $out = [];
    $rc = 0;
    exec($cmd . ' 2>&1', $out, $rc);
    $body = implode("\n", $out);

    echo "curl_rc=$rc\n";
    echo "body: $body\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
curl_rc=0
body: alpha, beta|a=1; b=2
Done
