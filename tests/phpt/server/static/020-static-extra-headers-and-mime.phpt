--TEST--
StaticHandler: setHeader + setMimeType applied through freeze/release lifecycle
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Exercises src/static/static_handler_class.c paths that previously
 * triggered a persistent-HashTable shutdown assertion:
 *   - L276-291: freeze extra_headers into shared snapshot
 *   - L297-313: freeze mime_overrides into shared snapshot
 *   - L388-397: release both at server destruction
 * Also exercises:
 *   - mount_resolve_content_type returning the override
 *   - apply_mount_headers emitting the extra header on 200 + 304 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/sh-eh-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/page.mdx", "# hello");
register_shutdown_function(function() use ($root) {
    @unlink("$root/page.mdx"); @rmdir($root);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$h = (new StaticHandler('/s/', $root))
    ->setMimeType('mdx', 'text/markdown; charset=utf-8')
    ->setHeader('X-Mount', 'A')
    ->setHeader('X-Custom-Tag', 'hello-world');
$server->addStaticHandler($h);

$client = spawn(function() use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }
    $do = function(array $extra = []) use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
        stream_set_timeout($fp, 2);
        $req = "GET /s/page.mdx HTTP/1.1\r\nHost: x\r\nConnection: close\r\n";
        foreach ($extra as $k => $v) $req .= "$k: $v\r\n";
        $req .= "\r\n";
        fwrite($fp, $req);
        $resp = '';
        while (!feof($fp)) {
            $c = fread($fp, 4096);
            if ($c === '' || $c === false) break;
            $resp .= $c;
        }
        fclose($fp);
        $he = strpos($resp, "\r\n\r\n");
        $head = $he === false ? $resp : substr($resp, 0, $he);
        $hd = [];
        $first = '';
        foreach (explode("\r\n", $head) as $i => $l) {
            if ($i === 0) { $first = $l; continue; }
            if (str_contains($l, ':')) {
                [$k, $v] = explode(':', $l, 2);
                $hd[strtolower(trim($k))] = trim($v);
            }
        }
        return [$first, $hd];
    };

    /* 200 path — mime override + extra headers applied. */
    [$st, $hd] = $do();
    echo "status:  $st\n";
    echo "ct:      ", $hd['content-type']  ?? '?', "\n";
    echo "x-mount: ", $hd['x-mount']       ?? '?', "\n";
    echo "x-tag:   ", $hd['x-custom-tag']  ?? '?', "\n";

    /* 304 path — extra headers must still apply. */
    $etag = $hd['etag'] ?? null;
    if ($etag !== null) {
        [$st304, $hd304] = $do(['If-None-Match' => $etag]);
        echo "304-st:  $st304\n";
        echo "304-mt:  ", $hd304['x-mount']      ?? '?', "\n";
        echo "304-tg:  ", $hd304['x-custom-tag'] ?? '?', "\n";
    } else {
        echo "304-st:  no-etag\n304-mt:  no-etag\n304-tg:  no-etag\n";
    }

    $server->stop();
});
$server->start();
await($client);
echo "done\n";
--EXPECT--
status:  HTTP/1.1 200 OK
ct:      text/markdown; charset=utf-8
x-mount: A
x-tag:   hello-world
304-st:  HTTP/1.1 304 Not Modified
304-mt:  A
304-tg:  hello-world
done
