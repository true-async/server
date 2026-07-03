--TEST--
StaticHandler: traversal-via-symlink, backslash, EACCES, header injection (issue #13)
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

/* Two-tree fixture: a public root and a restricted sibling. The
 * symlink test proves realpath-prefix-check rejects an inside-root
 * symlink that points outside. */
$base = sys_get_temp_dir() . '/static-sec-' . getmypid() . '-' . bin2hex(random_bytes(4));
$root = "$base/public";
$secret_dir = "$base/secret";
mkdir($root, 0700, true);
mkdir($secret_dir, 0700, true);
file_put_contents("$root/visible.txt", "ok");
file_put_contents("$secret_dir/secret.txt", "TOP-SECRET");
@symlink($secret_dir, "$root/sneak");

/* File the process can stat() but not read — simulates a chmod 600
 * file owned by another user. We can't always create one, so use
 * chmod 000 on a file we own (the running php process can stat but
 * read() returns EACCES on most Unixes). Whether or not the chmod
 * sticks (root user defeats it), the disclosure check still passes
 * because both branches map to 404. */
$forbidden = "$root/forbidden.txt";
file_put_contents($forbidden, "private");
chmod($forbidden, 0);

register_shutdown_function(function() use ($base, $forbidden) {
    @chmod($forbidden, 0644);
    /* recursive cleanup */
    $rm = function($p) use (&$rm) {
        if (is_link($p) || is_file($p)) { @unlink($p); return; }
        if (is_dir($p)) {
            foreach (scandir($p) as $e) {
                if ($e === '.' || $e === '..') continue;
                $rm("$p/$e");
            }
            @rmdir($p);
        }
    };
    $rm($base);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$h = new StaticHandler('/static/', $root);
$server->addStaticHandler($h);

/* setCacheControl rejects CRLF (defense-in-depth). */
$injected = false;
try {
    $bad = new StaticHandler('/x/', $root);
    $bad->setCacheControl("no-cache\r\nX-Injected: yes");
} catch (\TrueAsync\HttpServerInvalidArgumentException $e) {
    $injected = true;
}
echo "cache-control-injection-blocked: " . ($injected ? "yes" : "NO") . "\n";

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
        }
        fclose($fp);
        $he = strpos($resp, "\r\n\r\n");
        $head = $he === false ? $resp : substr($resp, 0, $he);
        return strtok($head, "\r\n");
    };

    /* Sanity: visible file works. */
    echo "visible: " . $do("GET /static/visible.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n") . "\n";

    /* Symlink → outside root must be 404 even when O_NOFOLLOW would
     * have permitted following an intermediate symlink. */
    echo "symlink-escape: " . $do("GET /static/sneak/secret.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n") . "\n";

    /* Backslash in URL must be rejected before reaching the FS
     * (Windows separator hardening). */
    echo "backslash-literal: " . $do("GET /static/..\\..\\etc\\passwd HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n") . "\n";

    /* Same via percent-encoded backslash (%5C). */
    echo "backslash-encoded: " . $do("GET /static/%5C..%5Cpasswd HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n") . "\n";

    /* %00 in URL — NUL injection. */
    echo "nul-injection: " . $do("GET /static/visible.txt%00.evil HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n") . "\n";

    /* Forbidden-but-existing file: 404, not 403, so existence isn't
     * disclosed. (When run as root chmod 000 has no effect — file
     * is read normally and we get 200; that's also fine, the test
     * is "no 403 leak", not "denied by chmod".) */
    $r = $do("GET /static/forbidden.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    $code_part = explode(' ', $r)[1] ?? '';
    echo "forbidden-not-403: " . ($code_part === '403' ? "LEAK" : "ok") . "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECTF--
cache-control-injection-blocked: yes
visible: HTTP/1.1 200 OK
symlink-escape: HTTP/1.1 404 Not Found
backslash-literal: HTTP/1.1 400 Bad Request
backslash-encoded: HTTP/1.1 400 Bad Request
nul-injection: HTTP/1.1 400 Bad Request
forbidden-not-403: ok
done
