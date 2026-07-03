--TEST--
StaticHandler: If-Modified-Since with past mtime → 304, future mtime → 200 (issue #13)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The static handler parses both If-None-Match (covered by 001) and
 * If-Modified-Since (RFC 9110 §13.1.3).  This test exercises the IMS
 * path: a date strictly later than the file's mtime must yield 304;
 * a date strictly earlier must yield 200. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-ims-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/page.html", "<h1>page</h1>");
/* Pin mtime so the test is deterministic regardless of when the
 * temp file was written (touch syscall has 1-second resolution on
 * some filesystems; pick a 2024 date so we have wide slack). */
$file_mtime = strtotime('2024-06-01 12:00:00 UTC');
touch("$root/page.html", $file_mtime);

register_shutdown_function(function() use ($root) {
    @unlink("$root/page.html");
    @rmdir($root);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler(new StaticHandler('/static/', $root));

$client = spawn(function() use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }

    $do = function(string $req) use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        stream_set_timeout($fp, 2);
        fwrite($fp, $req);
        $resp = '';
        while (!feof($fp)) {
            $chunk = fread($fp, 4096);
            if ($chunk === '' || $chunk === false) break;
            $resp .= $chunk;
            if (strlen($resp) > 16384) break;
        }
        fclose($fp);
        $he = strpos($resp, "\r\n\r\n");
        $head = $he === false ? $resp : substr($resp, 0, $he);
        return explode("\r\n", $head)[0] ?? '';
    };

    /* 1. IMS strictly AFTER the file's mtime → 304 (file unchanged
     *    since the client's reference). */
    $after  = gmdate('D, d M Y H:i:s \G\M\T', strtotime('2025-01-01 00:00:00 UTC'));
    $st = $do("GET /static/page.html HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: $after\r\nConnection: close\r\n\r\n");
    echo "after-mtime: $st\n";

    /* 2. IMS strictly BEFORE the file's mtime → 200 (file was
     *    modified since the client's reference). */
    $before = gmdate('D, d M Y H:i:s \G\M\T', strtotime('2020-01-01 00:00:00 UTC'));
    $st = $do("GET /static/page.html HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: $before\r\nConnection: close\r\n\r\n");
    echo "before-mtime: $st\n";

    /* 3. IMS equal to the file's mtime → 304 (RFC 9110: "later than
     *    the date in the If-Modified-Since header field" → strictly
     *    later, so equal → not modified). */
    $equal = gmdate('D, d M Y H:i:s \G\M\T', strtotime('2024-06-01 12:00:00 UTC'));
    $st = $do("GET /static/page.html HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: $equal\r\nConnection: close\r\n\r\n");
    echo "equal-mtime: $st\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
after-mtime: HTTP/1.1 304 Not Modified
before-mtime: HTTP/1.1 200 OK
equal-mtime: HTTP/1.1 304 Not Modified
done
