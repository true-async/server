--TEST--
StaticHandler: dotfile policy=IGNORE + directory-without-index branches
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Exercises src/static/http_static.c branches not covered by 002:
 *   - DOTFILES_IGNORE returns HTTP_STATIC_PATH_HIDE
 *       → default 404 (L199-200)
 *       → on_missing:Next passes through (L195-196)
 *   - Directory with no matching index file
 *       → default 404 (L255-256)
 *       → on_missing:Next passes through (L251-252)
 *
 * Two mounts cover the on_missing variants; a third mount with
 * DOTFILES_IGNORE + on_missing default covers the 404 arm. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use TrueAsync\StaticDotfiles;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/sh-noidx-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir("$root/empty", 0700, true);
file_put_contents("$root/.hidden", "secret");
file_put_contents("$root/visible.txt", "ok");
register_shutdown_function(function() use ($root) {
    @unlink("$root/.hidden");
    @unlink("$root/visible.txt");
    @rmdir("$root/empty");
    @rmdir($root);
});

$port   = 19960 + getmypid() % 30;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);

/* Mount A: dotfile=IGNORE, on_missing default → dotfile request 404. */
$a = (new StaticHandler('/a/', $root))->setDotfilePolicy(StaticDotfiles::IGNORE);
$server->addStaticHandler($a);

/* Mount B: dotfile=IGNORE, on_missing:Next → dotfile request falls
 * through to PHP. Also serves as the "dir without index" passthrough
 * mount — empty/ dir + Next → PHP. */
$b = (new StaticHandler('/b/', $root))
    ->setDotfilePolicy(StaticDotfiles::IGNORE)
    ->setOnMissing(StaticOnMissing::NEXT);
$server->addStaticHandler($b);

/* Mount C: default policies → dir without index → 404. */
$c = (new StaticHandler('/c/', $root));
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
    $get = function(string $path) use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
        stream_set_timeout($fp, 2);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $resp = '';
        while (!feof($fp)) {
            $c = fread($fp, 4096);
            if ($c === '' || $c === false) break;
            $resp .= $c;
        }
        fclose($fp);
        $head = substr($resp, 0, strpos($resp, "\r\n\r\n"));
        $body = substr($resp, strpos($resp, "\r\n\r\n") + 4);
        return [explode("\r\n", $head)[0], trim($body)];
    };

    /* IGNORE + default on_missing → dotfile 404. */
    [$st, $_] = $get('/a/.hidden');
    echo "ignore-default: $st\n";

    /* IGNORE + on_missing:Next → dotfile passthrough → PHP. */
    [$st, $bd] = $get('/b/.hidden');
    echo "ignore-next:    $st php=", str_contains($bd, 'PHP') ? 'yes' : 'no', "\n";

    /* Sanity: visible file under IGNORE mount still works. */
    [$st, $bd] = $get('/a/visible.txt');
    echo "ignore-visible: $st body=", $bd, "\n";

    /* Directory with no index → on_missing:Next mount → PHP. */
    [$st, $bd] = $get('/b/empty/');
    echo "dir-noidx-next: $st php=", str_contains($bd, 'PHP') ? 'yes' : 'no', "\n";

    /* Directory with no index → default mount → 404. */
    [$st, $_] = $get('/c/empty/');
    echo "dir-noidx-404:  $st\n";

    $server->stop();
});
$server->start();
await($client);
echo "done\n";
--EXPECT--
ignore-default: HTTP/1.1 404 Not Found
ignore-next:    HTTP/1.1 299 Unknown php=yes
ignore-visible: HTTP/1.1 200 OK body=ok
dir-noidx-next: HTTP/1.1 299 Unknown php=yes
dir-noidx-404:  HTTP/1.1 404 Not Found
done
