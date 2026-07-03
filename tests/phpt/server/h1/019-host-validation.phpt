--TEST--
HttpServer: HTTP/1.1 Host header validation (RFC 9112 §3.2 / 9110 §7.2)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!extension_loaded('sockets') && !function_exists('stream_socket_client')) {
    die('skip sockets required');
}
?>
--FILE--
<?php
/* RFC 9112 §3.2 / RFC 9110 §7.2: an HTTP/1.1 request carries exactly one
 * Host header whose value is host[:port] — no userinfo, path, comma list,
 * or empty value. Each rejected shape is a routing-confusion / request-
 * smuggling vector (surfaced by Http11Probe, issue #47). Also: an empty
 * Transfer-Encoding value is malformed (RFC 9112 §6.1). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

/* Send a raw request, return the numeric status from the response line. */
function probe(int $port, string $raw): int {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    if (!$fp) { return -1; }
    fwrite($fp, $raw);
    stream_set_timeout($fp, 2);
    $resp = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $resp .= $c;
        if (str_contains($resp, "\r\n")) break;
    }
    fclose($fp);
    if (preg_match('#^HTTP/1\.[01] (\d{3})#', $resp, $m)) { return (int)$m[1]; }
    return 0;
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $cases = [
        'valid'          => "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n",
        'valid_port'     => "GET / HTTP/1.1\r\nHost: example.com:8080\r\nConnection: close\r\n\r\n",
        'missing'        => "GET / HTTP/1.1\r\nConnection: close\r\n\r\n",
        'duplicate'      => "GET / HTTP/1.1\r\nHost: a.com\r\nHost: b.com\r\nConnection: close\r\n\r\n",
        'userinfo'       => "GET / HTTP/1.1\r\nHost: user@evil.com\r\nConnection: close\r\n\r\n",
        'path'           => "GET / HTTP/1.1\r\nHost: a.com/evil\r\nConnection: close\r\n\r\n",
        'empty'          => "GET / HTTP/1.1\r\nHost:\r\nConnection: close\r\n\r\n",
        'comma'          => "GET / HTTP/1.1\r\nHost: a.com, b.com\r\nConnection: close\r\n\r\n",
        'te_empty'       => "POST / HTTP/1.1\r\nHost: a.com\r\nTransfer-Encoding:\r\nConnection: close\r\n\r\n",
    ];
    foreach ($cases as $name => $raw) {
        echo "$name=", probe($port, $raw), "\n";
    }
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
valid=200
valid_port=200
missing=400
duplicate=400
userinfo=400
path=400
empty=400
comma=400
te_empty=400
done
