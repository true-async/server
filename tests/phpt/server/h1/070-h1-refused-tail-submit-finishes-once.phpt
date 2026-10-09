--TEST--
HTTP/1: a pipelined response tail whose submit is refused ends the connection once, without touching it after its destroy
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_fault_enable')) die('skip needs --enable-fault-injection');
?>
--FILE--
<?php
/* Two pipelined requests, the second asking for close. The first response is
 * in flight when the second is appended behind it, and the connection's
 * destroy waits for that write. Its completion submits the tail; the fault
 * point submits it with a length the reactor refuses, which runs the tail's
 * completion inside the submit. The connection must be finished and destroyed
 * once: a second finish after the nested one works on a freed connection.
 * The connection arena keeps a freed slot, so neither valgrind nor ASan sees
 * that read; the assertion at the top of the finish does, in a debug build
 * only. The client gets the first response and the close; a new connection is
 * still served. */
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$point = 'h1/write_tail/refused';
$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addHttpHandler(function ($req, $res) {
    /* 4 MiB for the first: more than the socket buffers hold, so its write
     * stays in flight until the client reads, and the second response is
     * appended behind it whatever the scheduling. */
    $size = $req->getPath() === '/one' ? 4 << 20 : 64;
    $res->setBody(str_repeat('b', $size) . $req->getPath());
});

function read_all($fp): string
{
    $got = '';

    while (!feof($fp)) {
        $chunk = fread($fp, 65536);

        if ($chunk === '' || $chunk === false) {
            break;
        }

        $got .= $chunk;
    }

    return $got;
}

$client = spawn(function () use ($port, $server, $point) {
    delay(80);
    _http_fault_enable($point);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 3);
    fwrite($fp, "GET /one HTTP/1.1\r\nHost: x\r\n\r\n"
              . "GET /two HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    delay(300);
    $got = read_all($fp);
    fclose($fp);

    $hits = _http_fault_hits($point);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 3);
    fwrite($fp, "GET /three HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    $after = read_all($fp);
    fclose($fp);

    $server->stop();

    return [$got, $hits, $after];
});

$server->start();
[$got, $hits, $after] = await($client);

echo "hits: $hits\n";
echo "first response: ", str_contains($got, '/one') ? 'yes' : 'no', "\n";
echo "second response: ", str_contains($got, '/two') ? 'yes' : 'no', "\n";
echo "next connection served: ", str_contains($after, '/three') ? 'yes' : 'no', "\n";
--EXPECT--
hits: 1
first response: yes
second response: no
next connection served: yes
