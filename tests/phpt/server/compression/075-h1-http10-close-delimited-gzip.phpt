--TEST--
Compression — a gzipped stream to an HTTP/1.0 client is delimited by the close
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
if (!function_exists('gzdecode')) die('skip zlib not available');
?>
--FILE--
<?php
/* Content coding and message framing are independent: a body may be gzipped and
 * still be delimited by the connection close, which is the only framing left
 * for an HTTP/1.0 peer whose response length is not known in advance.
 *
 * The two travel through different layers and this is where they meet — the
 * compressing wrapper feeds the encoder and hands one chunk to the transport,
 * which is what decides the framing. A wrapper that framed for itself would put
 * chunk size lines around the deflate blocks, and gzdecode() would refuse the
 * result.
 *
 * The 1.1 run is the control: same payload, same negotiation, chunked framing
 * around the same compressed bytes. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
);

$payload = str_repeat("compressible payload\n", 200);

$server->addHttpHandler(function ($req, $res) use ($payload) {
    $res->setHeader('Content-Type', 'text/html');

    /* The route where all three rules meet: a 1.0 peer that would otherwise
     * get a close-delimited body, an Accept-Encoding that would otherwise be
     * honoured, and a declared length that overrules both — compression is
     * refused because the count would describe the wrong bytes, the framing
     * is the count rather than the close, and the connection survives. */
    if ($req->getPath() === '/declared') {
        $res->setHeader('Content-Length', (string) strlen($payload));
    }

    $res->write($payload);
    $res->end();
});

spawn(function () use ($port, $server, $payload) {
    usleep(50000);

    $fetch = static function ($port, $request) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
        stream_set_timeout($fp, 3);
        fwrite($fp, $request);
        $all = '';

        while (!feof($fp)) {
            $chunk = fread($fp, 8192);

            if ($chunk === false || $chunk === '') {
                break;
            }

            $all .= $chunk;
        }

        fclose($fp);
        $split = strpos($all, "\r\n\r\n");

        return [substr($all, 0, $split), substr($all, $split + 4)];
    };

    /* Undo chunked framing so both runs are compared as the same bytes. */
    $dechunk = static function ($body) {
        $out = '';

        while (($nl = strpos($body, "\r\n")) !== false) {
            $size = hexdec(substr($body, 0, $nl));

            if ($size === 0) {
                break;
            }

            $out  .= substr($body, $nl + 2, $size);
            $body  = substr($body, $nl + 2 + $size + 2);
        }

        return $out;
    };

    [$head, $body] = $fetch($port,
        "GET /gz HTTP/1.0\r\nAccept-Encoding: gzip\r\n\r\n");

    echo "1.0 content-encoding: ",
        preg_match('/^content-encoding:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "1.0 transfer-encoding: ",
        preg_match('/^transfer-encoding:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "1.0 connection: ",
        preg_match('/^connection:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "1.0 compressed smaller: ", strlen($body) < strlen($payload) ? 'yes' : 'no', "\n";
    echo "1.0 decodes to payload: ", gzdecode($body) === $payload ? 'yes' : 'no', "\n";

    [$head, $body] = $fetch($port,
        "GET /gz HTTP/1.1\r\nHost: x\r\nConnection: close\r\nAccept-Encoding: gzip\r\n\r\n");

    echo "1.1 transfer-encoding: ",
        preg_match('/^transfer-encoding:/mi', $head) ? 'present' : '<absent>', "\n";
    echo "1.1 decodes to payload: ", gzdecode($dechunk($body)) === $payload ? 'yes' : 'no', "\n";

    [$head, $body] = $fetch($port,
        "GET /declared HTTP/1.0\r\nConnection: keep-alive\r\nAccept-Encoding: gzip\r\n\r\n");

    echo "declared content-encoding: ",
        preg_match('/^content-encoding:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "declared content-length: ",
        preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? (int) $m[1] : -1, "\n";
    echo "declared connection: ",
        preg_match('/^connection:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "declared body is payload: ", $body === $payload ? 'yes' : 'no', "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
1.0 content-encoding: gzip
1.0 transfer-encoding: <absent>
1.0 connection: close
1.0 compressed smaller: yes
1.0 decodes to payload: yes
1.1 transfer-encoding: present
1.1 decodes to payload: yes
declared content-encoding: <absent>
declared content-length: 4200
declared connection: keep-alive
declared body is payload: yes
