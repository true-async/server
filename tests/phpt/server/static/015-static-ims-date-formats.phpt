--TEST--
StaticHandler: If-Modified-Since alternate RFC 9110 date formats (RFC 850 + asctime)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* RFC 9110 §5.6.7 requires servers accept three date formats:
 *   - IMF-fixdate (covered by 005-static-if-modified-since)
 *   - RFC 850   "Sunday, 06-Jan-25 00:00:00 GMT"
 *   - asctime   "Sun Jan  6 00:00:00 2025"
 * Plus malformed dates → parser returns (time_t)-1 → header ignored. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-ims-fmt-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/p.html", "x");
$mtime = strtotime('2024-06-01 12:00:00 UTC');
touch("$root/p.html", $mtime);
register_shutdown_function(function() use ($root) {
    @unlink("$root/p.html");
    @rmdir($root);
});

$port = 20040 + getmypid() % 30;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler(new StaticHandler('/s/', $root));

$client = spawn(function() use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }
    $do = function(string $ims) use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
        stream_set_timeout($fp, 2);
        fwrite($fp, "GET /s/p.html HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: $ims\r\nConnection: close\r\n\r\n");
        $resp = '';
        while (!feof($fp)) {
            $c = fread($fp, 4096);
            if ($c === '' || $c === false) break;
            $resp .= $c;
        }
        fclose($fp);
        return explode("\r\n", $resp)[0];
    };

    /* 1. RFC 850 — after mtime → 304. Year < 69 → 20xx, else 19xx (parser
     *    uses the well-known two-digit-year pivot at 69). */
    echo "rfc850-after:   ", $do("Sunday, 01-Jan-25 00:00:00 GMT"), "\n";
    /* 2. RFC 850 — before mtime → 200. */
    echo "rfc850-before:  ", $do("Sunday, 01-Jan-70 00:00:00 GMT"), "\n";
    /* 3. asctime — after mtime → 304. Note the double space before
     *    single-digit day, which is canonical asctime output. */
    echo "asctime-after:  ", $do("Wed Jan  1 00:00:00 2025"), "\n";
    /* 4. asctime — before mtime → 200. */
    echo "asctime-before: ", $do("Thu Jan  1 00:00:00 1970"), "\n";
    /* 5. Garbage with a comma → RFC-850 branch, sscanf fails → parser
     *    returns -1 → handler ignores header → 200 (full response). */
    echo "garbage-comma:  ", $do("not, a date"), "\n";
    /* 6. Garbage no comma → asctime branch, sscanf fails → 200. */
    echo "garbage-plain:  ", $do("totally bogus"), "\n";
    /* 7. Out-of-range field (month=13) → tm_fields_in_range rejects
     *    → header ignored → 200. */
    echo "range-bad-mon:  ", $do("Sun, 01 Xyz 2025 00:00:00 GMT"), "\n";
    /* 8. Out-of-range hour (24) — IMF format parses, range check rejects. */
    echo "range-bad-hr:   ", $do("Sun, 01 Jan 2025 24:00:00 GMT"), "\n";

    $server->stop();
});
$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
rfc850-after:   HTTP/1.1 304 Not Modified
rfc850-before:  HTTP/1.1 200 OK
asctime-after:  HTTP/1.1 304 Not Modified
asctime-before: HTTP/1.1 200 OK
garbage-comma:  HTTP/1.1 200 OK
garbage-plain:  HTTP/1.1 200 OK
range-bad-mon:  HTTP/1.1 200 OK
range-bad-hr:   HTTP/1.1 200 OK
done
