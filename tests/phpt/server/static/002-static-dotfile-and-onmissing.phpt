--TEST--
StaticHandler: dotfile-deny default + on_missing: Next falls through to PHP (issue #13)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
mkdir("$root/.git", 0700, true);
file_put_contents("$root/.git/config", "[secret]");
file_put_contents("$root/visible.txt", "ok");

register_shutdown_function(function() use ($root) {
    @unlink("$root/.git/config");
    @rmdir("$root/.git");
    @unlink("$root/visible.txt");
    @rmdir($root);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);

$h = (new StaticHandler('/static/', $root))
    ->setOnMissing(StaticOnMissing::NEXT);
$server->addStaticHandler($h);

/* Catch-all: return a sentinel any time the static path falls through. */
$server->addHttpHandler(function($req, $res) {
    $res->setStatusCode(299)
        ->setBody("PHP saw " . $req->getUri())
        ->end();
});

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
        $body = $he === false ? '' : substr($resp, $he + 4);
        $line = explode("\r\n", $head)[0] ?? '';
        return [$line, trim($body)];
    };

    /* Static file is served by the static handler. */
    [$st, $bd] = $do("GET /static/visible.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "visible: $st body=$bd\n";

    /* Dotfile blocked by Deny default — but on_missing: Next means
     * "falls through to PHP", so PHP gets called. The contract: dotfile
     * deny == 404 in C without entering PHP regardless of on_missing.
     * That's the security guarantee — Next is for genuinely-missing
     * files, not for hiding access controls. */
    [$st, $bd] = $do("GET /static/.git/config HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "dotfile: $st\n";

    /* Missing file with on_missing: Next → PHP runs. */
    [$st, $bd] = $do("GET /static/no-such-file.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "next: $st body=$bd\n";

    /* URL outside the static prefix → PHP also runs. */
    [$st, $bd] = $do("GET /api/ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "outside: $st body=$bd\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECTF--
visible: HTTP/1.1 200 OK body=ok
dotfile: HTTP/1.1 404 Not Found
next: HTTP/1.1 299 %s body=PHP saw /static/no-such-file.txt
outside: HTTP/1.1 299 %s body=PHP saw /api/ping
done
