--TEST--
HttpRequest::readBody() — H1 streaming request body (issue #26)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!shell_exec('which curl')) die('skip curl not installed');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setBodyStreamingEnabled(true)
        ->setMaxBodySize(8 * 1024 * 1024)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $total  = 0;
    $chunks = 0;
    while (($c = $req->readBody()) !== null) {
        $total  += strlen($c);
        $chunks++;
    }
    // Second readBody() after EOF must be null (idempotent).
    $tail = $req->readBody();
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody("bytes=$total tail=" . ($tail === null ? 'null' : strlen($tail)));
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);
    // 64 KiB body — large enough to require multiple on_body callbacks.
    // Feed it via a temp file, not `echo -n | curl`: `echo -n` is not
    // portable (macOS /bin/sh emits extra bytes) and a 64 KiB shell
    // argument is fragile besides.
    $body = str_repeat('A', 64 * 1024);
    $tmp  = tempnam(sys_get_temp_dir(), 'body');
    file_put_contents($tmp, $body);
    $cmd  = sprintf(
        'curl --http1.1 -s --max-time 5 --data-binary @%s -H "Expect:" http://127.0.0.1:%d/upload',
        escapeshellarg($tmp), $port
    );
    $resp = shell_exec($cmd);
    unlink($tmp);
    echo $resp, "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
bytes=65536 tail=null
done
