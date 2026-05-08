--TEST--
StaticHandler: SymlinkPolicy::OwnerMatch follows owner-equal links (issue #13 §6)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* OwnerMatch was aliased to Reject in the MVP. The real implementation
 * walks every path segment, lstat's the link, stat's the target, and
 * rejects on uid mismatch. Verify it does follow same-owner symlinks
 * (the test process owns both the link and the target, so the sweep
 * should accept). Also verify the default Reject mode still 404s the
 * same path so the two policies stay observably different.
 *
 * Cross-owner mismatch can't be exercised without root (no way to
 * chown a file to a different uid as a regular user). We validate the
 * accept side here; the deny side is exercised by the existing
 * traversal-via-symlink test in 003-static-security.phpt under Reject
 * mode (intermediate symlinks pointing outside the root). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticSymlinks;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-owner-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/real.txt", "REAL");
@symlink("$root/real.txt", "$root/alias.txt");

register_shutdown_function(function() use ($root) {
    @unlink("$root/alias.txt");
    @unlink("$root/real.txt");
    @rmdir($root);
});

$port = 19960 + getmypid() % 9;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);

/* Two mounts on the same files: one in OwnerMatch, one in Reject. */
$server->addStaticHandler(
    (new StaticHandler('/owner/', $root))
        ->disableIndex()
        ->setSymlinkPolicy(StaticSymlinks::OwnerMatch)
);
$server->addStaticHandler(
    (new StaticHandler('/reject/', $root))
        ->disableIndex()
        ->setSymlinkPolicy(StaticSymlinks::Reject)
);

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
        $head = explode("\r\n", $resp)[0] ?? '';
        $body = strstr($resp, "\r\n\r\n");
        $body = $body !== false ? substr($body, 4) : '';
        return [$head, $body];
    };

    /* OwnerMatch: same-owner symlink is followed, real file via
     * non-symlink path also works. Reject: same path through the
     * symlink is 404, direct non-symlink path is 200. */
    [$h, $b] = $do("GET /owner/real.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "owner-direct: $h body=$b\n";
    [$h, $b] = $do("GET /owner/alias.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "owner-via-link: $h body=$b\n";
    [$h, $b] = $do("GET /reject/real.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "reject-direct: $h body=$b\n";
    [$h, ]   = $do("GET /reject/alias.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    echo "reject-via-link: $h\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
owner-direct: HTTP/1.1 200 OK body=REAL
owner-via-link: HTTP/1.1 200 OK body=REAL
reject-direct: HTTP/1.1 200 OK body=REAL
reject-via-link: HTTP/1.1 404 Not Found
done
