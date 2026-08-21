--TEST--
A compressed stream whose handler forgets end() still closes its codec stream
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php if (!extension_loaded('zlib')) die('skip zlib required'); ?>
--FILE--
<?php
/* Two finishers used to disagree about where a stream ends. The handler's
 * end() went through the compressing wrapper, which closes the deflate stream
 * and emits its trailer; falling out of the handler instead reached the
 * transport underneath and wrote the chunked terminator by hand. The client
 * then read a body that was complete by every framing rule it could check and
 * undecodable by the only one that mattered: gzip's own.
 *
 * The assertion is the decoder's, not ours. gzdecode() returns false on a
 * stream without its trailer, and the length it returns otherwise is the whole
 * body — a trailer written over a short body would decode to less. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(5)
);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    $res->write(str_repeat('alpha', 200));
    $res->write(str_repeat('beta', 200));
    /* no end(): the dispose path finishes the response */
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 3);
    fwrite($fp, "GET /s HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n");

    $raw = '';

    while (!feof($fp)) {
        $piece = fread($fp, 8192);

        if ($piece === false || $piece === '') {
            break;
        }

        $raw .= $piece;
    }

    fclose($fp);

    $head_end = strpos($raw, "\r\n\r\n");
    $head     = substr($raw, 0, $head_end);
    $body     = substr($raw, $head_end + 4);

    /* De-chunk by hand: the point of the test is what the chunks carry. */
    $gz = '';
    $terminated = false;
    $at = 0;

    while ($at < strlen($body)) {
        $nl = strpos($body, "\r\n", $at);

        if ($nl === false) {
            break;
        }

        $size = hexdec(substr($body, $at, $nl - $at));

        if ($size === 0) {
            $terminated = true;
            break;
        }

        $gz .= substr($body, $nl + 2, $size);
        $at = $nl + 2 + $size + 2;
    }

    $plain = @gzdecode($gz);

    echo "gzip: ", (int) (stripos($head, 'content-encoding: gzip') !== false), "\n";
    echo "terminator: ", (int) $terminated, "\n";
    echo "decoded: ", $plain === false ? 'FAILED' : strlen($plain), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
gzip: 1
terminator: 1
decoded: 1800
