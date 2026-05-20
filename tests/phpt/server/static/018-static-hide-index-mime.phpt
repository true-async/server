--TEST--
StaticHandler: hide globs, index-file fallback, multi-mount, browse no-op
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Exercises http_static.c branches not covered by 001-013:
 *   - hide-glob match → 404 (default) + → PASSTHROUGH (on_missing: Next)
 *   - multi-mount registration (two StaticHandlers on the same server)
 *   - index-file iteration when first candidates miss
 *   - setBrowseEnabled(true) — currently a no-op accepted at setter
 *
 * setHeader / setMimeType are deliberately NOT exercised on an attached
 * handler here — they trigger a latent persistent-HashTable shutdown
 * crash (extra_headers / mime_overrides use ZVAL_PTR_DTOR on a
 * persistent table). See 014/015 for offline coverage of those setters. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use function Async\spawn;
use function Async\await;

$root1 = sys_get_temp_dir() . '/sh-hide-' . getmypid() . '-' . bin2hex(random_bytes(4));
$root2 = sys_get_temp_dir() . '/sh-other-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir("$root1/sub", 0700, true);
mkdir($root2, 0700, true);
file_put_contents("$root1/secret.bak",    "should be hidden");
file_put_contents("$root1/file.txt",      "plain");
file_put_contents("$root1/sub/home.html", "<h1>home</h1>");
file_put_contents("$root2/other.txt",     "second-mount");

register_shutdown_function(function() use ($root1, $root2) {
    foreach (["$root1/secret.bak", "$root1/file.txt", "$root1/sub/home.html",
              "$root2/other.txt"] as $f) @unlink($f);
    @rmdir("$root1/sub"); @rmdir($root1); @rmdir($root2);
});

$port   = 19880 + getmypid() % 30;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);

/* First mount: /a/ — hide *.bak, browse on, index list with first-miss
 * → second-hit fallback. */
$a = (new StaticHandler('/a/', $root1))
    ->hide('*.bak')
    ->setBrowseEnabled(true)
    ->setIndexFiles('missing.html', 'home.html');
$server->addStaticHandler($a);

/* Second mount: /b/ — separate root, on_missing: Next.
 * Multi-mount path exercises the mi>0 dispatch arm. */
$b = (new StaticHandler('/b/', $root2))
    ->setOnMissing(StaticOnMissing::NEXT);
$server->addStaticHandler($b);

/* Third mount: /c/ — hide *.bak + on_missing: Next.
 * Hide-match on this mount yields PASSTHROUGH instead of 404. */
$c = (new StaticHandler('/c/', $root1))
    ->hide('*.bak')
    ->setOnMissing(StaticOnMissing::NEXT);
$server->addStaticHandler($c);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(299)->setBody("PHP " . $req->getUri())->end();
});

$client = spawn(function() use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }
    $do = function(string $path, array $headers = []) use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
        stream_set_timeout($fp, 2);
        $req = "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n";
        foreach ($headers as $k => $v) $req .= "$k: $v\r\n";
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
        $body = $he === false ? '' : substr($resp, $he + 4);
        $hd = [];
        $first = '';
        foreach (explode("\r\n", $head) as $i => $l) {
            if ($i === 0) { $first = $l; continue; }
            if (str_contains($l, ':')) {
                [$k, $v] = explode(':', $l, 2);
                $hd[strtolower(trim($k))] = trim($v);
            }
        }
        return [$first, $hd, trim($body)];
    };

    /* 1. Hidden by glob → 404 (default on_missing) */
    [$st, $_, $_] = $do('/a/secret.bak');
    echo "hide-default: $st\n";

    /* 2. Hidden by glob on mount C (on_missing:Next) → PHP fallthrough */
    [$st, $_, $bd] = $do('/c/secret.bak');
    echo "hide-next:    $st body=", str_contains($bd, 'PHP') ? 'PHP' : 'other', "\n";

    /* 3. Plain file served from mount A. */
    [$st, $hd, $_] = $do('/a/file.txt');
    echo "plain:        $st\n";

    /* 4. Index fallback: GET /a/sub/ → missing.html fails → home.html succeeds */
    [$st, $hd, $bd] = $do('/a/sub/');
    echo "index-2nd:    $st body=", trim($bd), "\n";

    /* 5. 304 path: re-fetch with If-None-Match. */
    [$_, $hd, $_] = $do('/a/file.txt');
    $etag = $hd['etag'] ?? null;
    if ($etag !== null) {
        [$st, $hd304, $_] = $do('/a/file.txt', ['If-None-Match' => $etag]);
        echo "304-status:   $st\n";
    } else {
        echo "304-status:   no-etag\n";
    }

    /* 6. Second mount serves its own file */
    [$st, $_, $bd] = $do('/b/other.txt');
    echo "mount-b:      $st body=", trim($bd), "\n";

    /* 7. Mount B falls through (no match) — PHP runs */
    [$st, $_, $bd] = $do('/b/no-such');
    echo "mount-b-miss: $st body=", str_contains($bd, 'PHP') ? 'PHP' : 'other', "\n";

    /* 8. Outside any prefix → PHP */
    [$st, $_, $bd] = $do('/z/foo');
    echo "outside:      $st body=", str_contains($bd, 'PHP') ? 'PHP' : 'other', "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
hide-default: HTTP/1.1 404 Not Found
hide-next:    HTTP/1.1 299 Unknown body=PHP
plain:        HTTP/1.1 200 OK
index-2nd:    HTTP/1.1 200 OK body=<h1>home</h1>
304-status:   HTTP/1.1 304 Not Modified
mount-b:      HTTP/1.1 200 OK body=second-mount
mount-b-miss: HTTP/1.1 299 Unknown body=PHP
outside:      HTTP/1.1 299 Unknown body=PHP
done
