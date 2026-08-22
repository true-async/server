--TEST--
HTTP/2 h2c: a bad connection preface is answered with one GOAWAY and nothing else
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* nghttp2 cannot queue a GOAWAY once BAD_CLIENT_MAGIC has fired, so the server
 * sends a hand-built template — 17 bytes, PROTOCOL_ERROR — and drops the
 * connection. What must not follow it is the SETTINGS the session queued before
 * the preface failed: the peer has just been told the connection is dead, and
 * on this path it is often not an HTTP/2 speaker at all. */

require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', $port)->setReadTimeout(5)
);
$server->addHttpHandler(function ($req, $res) {
    $res->setBody('ok')->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);
    stream_set_timeout($fp, 3);

    /* The preface's first line is right, so the connection is routed to HTTP/2;
     * its second half is not, so nghttp2 refuses the client magic. */
    fwrite($fp, "PRI * HTTP/2.0\r\n\r\nXX\r\n\r\n");

    $wire = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 4096);

        if ($chunk === false || $chunk === '') { break; }

        $wire .= $chunk;
    }

    fclose($fp);

    echo 'bytes: ', strlen($wire), "\n";
    echo 'wire: ', bin2hex($wire), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
bytes: 17
wire: 0000080700000000000000000000000001
done
