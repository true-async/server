--TEST--
HttpServer: method + response conformance — CONNECT, asterisk-form, HEAD, Date (#47)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('stream_socket_client')) die('skip sockets required');
?>
--FILE--
<?php
/* RFC 9110/9112 conformance gaps surfaced by Http11Probe (#47):
 *   - CONNECT to an origin server → 405 (no tunnel)
 *   - asterisk-form "*" target is OPTIONS-only → 400 for GET, 200 for OPTIONS
 *   - HEAD response carries the Content-Length of the would-be body but
 *     no message body
 *   - every response includes a Date header (RFC 9110 §6.6.1) */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)->setReadTimeout(5)->setWriteTimeout(5));
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('hello-body');   /* 10 bytes */
});

function probe(int $port, string $raw): string {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    if (!$fp) { return ''; }
    fwrite($fp, $raw);
    stream_set_timeout($fp, 2);
    $r = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $r .= $c;
    }
    fclose($fp);
    return $r;
}
function status(string $r): int {
    return preg_match('#^HTTP/1\.[01] (\d{3})#', $r, $m) ? (int)$m[1] : 0;
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $h = "Host: a.com\r\nConnection: close\r\n\r\n";

    echo "connect=",      status(probe($port, "CONNECT a.com:443 HTTP/1.1\r\n$h")), "\n";
    echo "asterisk_get=",  status(probe($port, "GET * HTTP/1.1\r\n$h")), "\n";
    echo "asterisk_opts=", status(probe($port, "OPTIONS * HTTP/1.1\r\n$h")), "\n";
    echo "http12=",        status(probe($port, "GET / HTTP/1.2\r\n$h")), "\n";

    $get  = probe($port, "GET / HTTP/1.1\r\n$h");
    echo "get_has_date=", (stripos($get, "\r\ndate:") !== false ? 1 : 0), "\n";

    $head = probe($port, "HEAD / HTTP/1.1\r\n$h");
    $sep  = strpos($head, "\r\n\r\n");
    $body = $sep === false ? '' : substr($head, $sep + 4);
    echo "head_status=", status($head), "\n";
    echo "head_has_cl10=", (stripos($head, "content-length: 10") !== false ? 1 : 0), "\n";
    echo "head_body_empty=", ($body === '' ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
connect=405
asterisk_get=400
asterisk_opts=200
http12=400
get_has_date=1
head_status=200
head_has_cl10=1
head_body_empty=1
done
