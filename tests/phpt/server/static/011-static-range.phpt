--TEST--
StaticHandler: single Byte-Range support (issue #13 PR #3, RFC 9110 §14.2)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Single byte-range slicing: 206 with Content-Range + sliced body for
 * valid ranges, 416 for unsatisfiable, 200 fall-through for multi-range
 * (we ignore by spec permission). suffix-length form (`bytes=-N`) and
 * open-end form (`bytes=N-`) both covered. If-Range strong-match also
 * exercised. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-range-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
$body = '';
for ($i = 0; $i < 1000; $i++) $body .= chr(($i & 0x3f) + 0x40);   // 1000 printable bytes
file_put_contents("$root/data.bin", $body);

register_shutdown_function(function() use ($root) {
    @unlink("$root/data.bin");
    @rmdir($root);
});

$port = 19920 + getmypid() % 9;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler((new StaticHandler('/r/', $root))->disableIndex());

$client = spawn(function() use ($port, $server, $body) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }

    $do = function(string $hdrs) use ($port) {
        $req = "GET /r/data.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n$hdrs\r\n";
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        stream_set_timeout($fp, 2);
        fwrite($fp, $req);
        $resp = '';
        while (!feof($fp)) {
            $chunk = fread($fp, 4096);
            if ($chunk === '' || $chunk === false) break;
            $resp .= $chunk;
        }
        fclose($fp);
        $head_end = strpos($resp, "\r\n\r\n");
        $head = substr($resp, 0, $head_end);
        $body = substr($resp, $head_end + 4);
        $lines = explode("\r\n", $head);
        $status = $lines[0];
        $h = [];
        for ($i = 1; $i < count($lines); $i++) {
            if (preg_match('/^([^:]+):\s*(.*)$/', $lines[$i], $m)) {
                $h[strtolower($m[1])] = $m[2];
            }
        }
        return [$status, $h, $body];
    };

    /* Pull etag for the If-Range test. */
    [, $h0, ] = $do("");
    $etag = $h0['etag'] ?? '';

    [$s, $h, $b] = $do("Range: bytes=0-9\r\n");
    echo "first-10 status=$s cr=", ($h['content-range'] ?? '-'),
         " cl=", ($h['content-length'] ?? '-'),
         " body=", bin2hex($b), "\n";
    /* Expected slice: bytes 0..9 of $body. */

    [$s, $h, $b] = $do("Range: bytes=990-\r\n");
    echo "open-end status=$s cr=", ($h['content-range'] ?? '-'),
         " body=", bin2hex($b), "\n";

    [$s, $h, $b] = $do("Range: bytes=-5\r\n");
    echo "suffix-5 status=$s cr=", ($h['content-range'] ?? '-'),
         " body=", bin2hex($b), "\n";

    [$s, $h, $b] = $do("Range: bytes=2000-3000\r\n");
    echo "unsatisfiable status=$s cr=", ($h['content-range'] ?? '-'),
         " ar=", ($h['accept-ranges'] ?? '-'), "\n";

    [$s, $h, $b] = $do("Range: bytes=0-9,20-29\r\n");
    echo "multi-range status=$s cr=", ($h['content-range'] ?? '-'),
         " cl=", ($h['content-length'] ?? '-'), "\n";

    [$s, $h, $b] = $do("Range: bytes=0-9\r\nIf-Range: $etag\r\n");
    echo "if-range-match status=$s cr=", ($h['content-range'] ?? '-'), "\n";

    [$s, $h, $b] = $do("Range: bytes=0-9\r\nIf-Range: \"wrong-etag-1234\"\r\n");
    echo "if-range-miss status=$s cr=", ($h['content-range'] ?? '-'),
         " cl=", ($h['content-length'] ?? '-'), "\n";

    $server->stop();
});

$server->start();
await($client);

/* Assertions baked in: print expected snippets too. */
echo "expect-first-10=", bin2hex(substr($body, 0, 10)), "\n";
echo "expect-suffix-5=", bin2hex(substr($body, -5)), "\n";
echo "done\n";
--EXPECTF--
first-10 status=HTTP/1.1 206 Partial Content cr=bytes 0-9/1000 cl=10 body=%s
open-end status=HTTP/1.1 206 Partial Content cr=bytes 990-999/1000 body=%s
suffix-5 status=HTTP/1.1 206 Partial Content cr=bytes 995-999/1000 body=%s
unsatisfiable status=HTTP/1.1 416 Range Not Satisfiable cr=bytes */1000 ar=-
multi-range status=HTTP/1.1 200 OK cr=- cl=1000
if-range-match status=HTTP/1.1 206 Partial Content cr=bytes 0-9/1000
if-range-miss status=HTTP/1.1 200 OK cr=- cl=1000
expect-first-10=%s
expect-suffix-5=%s
done
