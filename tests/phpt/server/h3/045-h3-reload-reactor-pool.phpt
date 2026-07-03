--TEST--
HttpServer: HTTP/3 through the reactor pool survives a worker rotation (#93)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* Reactor-pool (#80) + hot reload (#93). The C transport reactors own the H3
 * listeners and outlive the PHP worker rotation. A retiring worker unpublishes
 * its inbox from the registry, fences every reactor (so no dispatch can still
 * hold the pointer), and waits out its mailbox tail; the replacements publish
 * fresh inboxes into the reused slots. An H3 GET must succeed both before and
 * after HttpServer::reload(). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$tmp = __DIR__ . '/tmp-045';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port     = tas_free_port();
$tcp_port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $tcp_port)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(201)
        ->setHeader('content-type', 'text/plain; charset=utf-8')
        ->setBody('echo:' . $req->getMethod() . ':' . $req->getUri());
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin) {
    usleep(600000);   /* reactors + workers thread up and bind */

    $get = static function (string $path) use ($client_bin, $port): string {
        $cmd = sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d %s GET 2>&1',
            escapeshellarg($client_bin), $port, $path);
        $out = shell_exec($cmd) ?? '';
        $status = preg_match('/^STATUS=(\d+)$/m', $out, $m) ? (int)$m[1] : -1;
        $body = trim((string) preg_replace('/^STATUS=\d+\n?/m', '', $out));
        return $status . ':' . $body;
    };

    echo "before=", $get('/one'), "\n";

    $ok = $server->reload();
    echo "reload=", var_export($ok, true), "\n";

    /* Replacements may still be booting/publishing — retry the H3 GET. */
    $after = '';
    for ($i = 0; $i < 30 && $after !== '201:echo:GET:/two'; $i++) {
        usleep(300000);
        $after = $get('/two');
    }

    echo "after=", $after, "\n";

    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
%Abefore=201:echo:GET:/one
%A
reload=true
%A
after=201:echo:GET:/two
%A
