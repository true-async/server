--TEST--
StaticHandler: 200, MIME, ETag, conditional GET, 404, traversal, HEAD (issue #13)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

/* Build a temporary docroot and spin up a server with one mount. */
$root = sys_get_temp_dir() . '/static-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/index.html", "<h1>root index</h1>");
file_put_contents("$root/style.css",  "body{color:red}");
mkdir("$root/sub", 0700, true);
file_put_contents("$root/sub/page.html", "nested");

register_shutdown_function(function() use ($root) {
    @unlink("$root/index.html");
    @unlink("$root/style.css");
    @unlink("$root/sub/page.html");
    @rmdir("$root/sub");
    @rmdir($root);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$h = (new StaticHandler('/static/', $root))->setIndexFiles('index.html');
$server->addStaticHandler($h);

$client = spawn(function() use ($port, $server) {
    /* Wait for the listener to be reachable. */
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
        $head_end = strpos($resp, "\r\n\r\n");
        $head = $head_end === false ? $resp : substr($resp, 0, $head_end);
        $body = $head_end === false ? '' : substr($resp, $head_end + 4);
        $lines = explode("\r\n", $head);
        $status = $lines[0] ?? '';
        $headers = [];
        for ($i = 1; $i < count($lines); $i++) {
            if (str_contains($lines[$i], ':')) {
                [$k, $v] = explode(':', $lines[$i], 2);
                $headers[strtolower(trim($k))] = trim($v);
            }
        }
        return [$status, $headers, $body];
    };

    [$st, $hd, $bd] = $do("GET /static/style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "css: $st ct={$hd['content-type']} body=" . trim($bd) . "\n";
    $etag = $hd['etag'];
    $has_lm = isset($hd['last-modified']) && $hd['last-modified'] !== '';
    echo "css-has-last-modified=" . ($has_lm ? "yes" : "no") . "\n";

    [$st, $hd, $bd] = $do("GET /static/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "idx: $st ct={$hd['content-type']} body=" . trim($bd) . "\n";

    [$st, $hd, $bd] = $do("GET /static/sub/page.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "sub: $st body=" . trim($bd) . "\n";

    [$st, $hd, $bd] = $do("GET /static/missing.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "miss: $st\n";

    [$st, $hd, $bd] = $do("GET /static/../../etc/passwd HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "trav-rel: $st\n";

    [$st, $hd, $bd] = $do("GET /static/%2e%2e/etc/passwd HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "trav-pct: $st\n";

    [$st, $hd, $bd] = $do("HEAD /static/style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "head: $st cl={$hd['content-length']} body-len=" . strlen($bd) . "\n";

    [$st, $hd, $bd] = $do("GET /static/style.css HTTP/1.1\r\nHost: x\r\nIf-None-Match: $etag\r\nConnection: close\r\n\r\n");
    echo "cond: $st body-len=" . strlen($bd) . "\n";

    /* Method other than GET/HEAD: passthrough → 404 (no PHP handler). */
    [$st, $hd, $bd] = $do("POST /static/style.css HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    echo "post: $st\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECTF--
css: HTTP/1.1 200 OK ct=text/css; charset=utf-8 body=body{color:red}
css-has-last-modified=yes
idx: HTTP/1.1 200 OK ct=text/html; charset=utf-8 body=<h1>root index</h1>
sub: HTTP/1.1 200 OK body=nested
miss: HTTP/1.1 404 Not Found
trav-rel: HTTP/1.1 404 Not Found
trav-pct: HTTP/1.1 404 Not Found
head: HTTP/1.1 200 OK cl=15 body-len=0
cond: HTTP/1.1 304 Not Modified body-len=0
post: HTTP/1.1 404 Not Found
done
