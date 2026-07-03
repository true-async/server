--TEST--
HttpServer: HTTP/1 TCP fragmentation — request split across multiple writes (issue #28+)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Regression guard: a request delivered as multiple TCP fragments (slow
 * client, tiny MTU, deliberate split) must reassemble before the handler
 * runs. The historical bug: dispatch fired in on_headers_complete, so the
 * handler saw a partial body via $req->getBody() and closed the connection
 * before the tail body bytes arrived — peer got Broken Pipe. After the
 * fix dispatch is deferred to on_message_complete for buffered bodies.
 *
 * Five cases exercise:
 *   1. split inside the request line (URL halves)
 *   2. split between request line and headers
 *   3. split between headers and body (CL body)
 *   4. split inside body (CL body, two halves)
 *   5. split chunked body (chunk size, chunk data, terminator) */

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
         ->setHeader('Content-Type', 'text/plain')
         ->setBody("uri=" . $req->getUri() . " body=" . $req->getBody());
});

function send_frags(int $port, array $frags, int $gap_us = 30000): string {
    $sock = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$sock) return "connect-fail: $errstr";
    stream_set_timeout($sock, 3);
    stream_set_blocking($sock, true);
    $last = count($frags) - 1;
    foreach ($frags as $i => $f) {
        if (@fwrite($sock, $f) === false) break;
        if ($i < $last) usleep($gap_us);
    }
    $data = '';
    while (!feof($sock)) {
        $chunk = @fread($sock, 8192);
        if ($chunk === false || $chunk === '') break;
        $data .= $chunk;
    }
    @fclose($sock);
    return $data;
}

function check(string $label, string $resp, string $needle): void {
    $ok = str_contains($resp, "200 OK") && str_contains($resp, $needle);
    echo "$label: ", ($ok ? "ok" : "FAIL\n--- response ---\n$resp\n---"), "\n";
}

$client = spawn(function() use ($port, $server) {
    usleep(50000);

    // 1: split request line mid-path
    $r = send_frags($port, [
        "GET /baseli",
        "ne11?a=1&b=2 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
    ]);
    check("case1 split-request-line", $r, "uri=/baseline11?a=1&b=2 body=");

    // 2: split between request line and headers
    $r = send_frags($port, [
        "GET /two HTTP/1.1\r\n",
        "Host: x\r\nConnection: close\r\n\r\n",
    ]);
    check("case2 split-headers", $r, "uri=/two body=");

    // 3: split between headers and body (CL body)
    $r = send_frags($port, [
        "POST /three HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\n",
        "hello",
    ]);
    check("case3 split-before-body", $r, "uri=/three body=hello");

    // 4: split mid-body (CL=8 in two halves)
    $r = send_frags($port, [
        "POST /four HTTP/1.1\r\nHost: x\r\nContent-Length: 8\r\nConnection: close\r\n\r\n",
        "abcd",
        "efgh",
    ]);
    check("case4 split-mid-body", $r, "uri=/four body=abcdefgh");

    // 5: chunked body split across writes
    $r = send_frags($port, [
        "POST /five HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
        "3\r\nabc\r\n",
        "4\r\ndefg\r\n",
        "0\r\n\r\n",
    ]);
    check("case5 split-chunked", $r, "uri=/five body=abcdefg");

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
case1 split-request-line: ok
case2 split-headers: ok
case3 split-before-body: ok
case4 split-mid-body: ok
case5 split-chunked: ok
Done
