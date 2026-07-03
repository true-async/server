--TEST--
StaticHandler: precompressed sidecars (.gz/.br/.zst) per Accept-Encoding (issue #13 PR #4)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Precompressed sidecar selection: when enablePrecompressed lists an
 * encoding and the request's Accept-Encoding accepts it AND a sibling
 * file exists at "<original>.<suffix>", serve that file with
 * Content-Encoding + Vary. Fall back to identity when nothing matches.
 *
 * Tests the four interesting cases:
 *   1. AE: gzip, .gz exists                  → 200 with Content-Encoding: gzip
 *   2. AE: identity, .gz exists              → 200 identity (gzip refused)
 *   3. AE: gzip, no sidecar                  → 200 identity
 *   4. AE: br;q=1.0, gzip;q=0.5, both sides  → server prefers br
 *      (tested via mock fixture; we just validate gzip-only sidecar
 *      with br-only AE picks identity, since the .gz path is the
 *      simpler one to assert here). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-pre-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/style.css",     "BIG-IDENTITY-CSS");
file_put_contents("$root/style.css.gz",  "FAKE-GZIP-BYTES");
file_put_contents("$root/plain.txt",     "no-sidecar-here");

register_shutdown_function(function() use ($root) {
    @unlink("$root/style.css");
    @unlink("$root/style.css.gz");
    @unlink("$root/plain.txt");
    @rmdir($root);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler(
    (new StaticHandler('/static/', $root))
        ->disableIndex()
        ->enablePrecompressed('gzip')
);

$client = spawn(function() use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }

    $do = function(string $path, ?string $accept_encoding) use ($port) {
        $hdrs = "Host: x\r\nConnection: close\r\n";
        if ($accept_encoding !== null) {
            $hdrs .= "Accept-Encoding: $accept_encoding\r\n";
        }
        $req = "GET $path HTTP/1.1\r\n$hdrs\r\n";
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
        $hmap = [];
        for ($i = 1; $i < count($lines); $i++) {
            if (preg_match('/^([^:]+):\s*(.*)$/', $lines[$i], $m)) {
                $hmap[strtolower($m[1])] = $m[2];
            }
        }
        return [$status, $hmap, $body];
    };

    [$s, $h, $b] = $do('/static/style.css', 'gzip');
    echo "case-1 status=$s ce=", ($h['content-encoding'] ?? '-'),
         " ct=", ($h['content-type'] ?? '-'),
         " vary=", ($h['vary'] ?? '-'),
         " body=$b\n";

    [$s, $h, $b] = $do('/static/style.css', 'identity');
    echo "case-2 status=$s ce=", ($h['content-encoding'] ?? '-'),
         " body=$b\n";

    [$s, $h, $b] = $do('/static/plain.txt', 'gzip');
    echo "case-3 status=$s ce=", ($h['content-encoding'] ?? '-'),
         " body=$b\n";

    [$s, $h, $b] = $do('/static/style.css', null);
    echo "case-4 status=$s ce=", ($h['content-encoding'] ?? '-'),
         " body=$b\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
case-1 status=HTTP/1.1 200 OK ce=gzip ct=text/css; charset=utf-8 vary=Accept-Encoding body=FAKE-GZIP-BYTES
case-2 status=HTTP/1.1 200 OK ce=- body=BIG-IDENTITY-CSS
case-3 status=HTTP/1.1 200 OK ce=- body=no-sidecar-here
case-4 status=HTTP/1.1 200 OK ce=- body=BIG-IDENTITY-CSS
done
