--TEST--
HttpServer: request-target + singleton-header hardening (fragment, backslash, dup Content-Type)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('stream_socket_client')) die('skip sockets required');
?>
--FILE--
<?php
/* RFC hardening (issue #47, optional strictness):
 *   - raw '#' in the request-target (a fragment, never sent on the wire,
 *     RFC 9112 §3.2) → 400
 *   - raw '\' in the request-target (not a URI char, RFC 3986; '\' vs '/'
 *     is parsed inconsistently → path confusion) → 400
 *   - duplicate Content-Type (RFC 9110 §8.3 singleton) → 400
 * Percent-encoded %23 / %5C are legal and must still pass. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)->setReadTimeout(5)->setWriteTimeout(5));
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

function probe(int $port, string $raw): int {
    $fp = false;
    for ($t = 0; $t < 20 && $fp === false; $t++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
        if ($fp === false) { usleep(10000); }
    }
    if ($fp === false) { return -1; }
    fwrite($fp, $raw);
    stream_set_timeout($fp, 2);
    $r = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $r .= $c;
        if (str_contains($r, "\r\n")) break;
    }
    fclose($fp);
    return preg_match('#^HTTP/1\.[01] (\d{3})#', $r, $m) ? (int)$m[1] : 0;
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $h = "Host: a.com\r\nConnection: close\r\n\r\n";
    echo "fragment=",   probe($port, "GET /p#frag HTTP/1.1\r\n$h"), "\n";
    echo "backslash=",  probe($port, "GET /a\\b HTTP/1.1\r\n$h"), "\n";
    echo "dup_ct=",      probe($port,
        "POST / HTTP/1.1\r\nHost: a.com\r\nContent-Type: text/a\r\n"
        . "Content-Type: text/b\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"), "\n";
    echo "encoded_hash=", probe($port, "GET /p%23frag HTTP/1.1\r\n$h"), "\n";
    echo "normal=",       probe($port, "GET /ok?x=1 HTTP/1.1\r\n$h"), "\n";
    echo "single_ct=",    probe($port,
        "POST / HTTP/1.1\r\nHost: a.com\r\nContent-Type: text/a\r\n"
        . "Content-Length: 0\r\nConnection: close\r\n\r\n"), "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
fragment=400
backslash=400
dup_ct=400
encoded_hash=200
normal=200
single_ct=200
done
