--TEST--
HttpServer: handlers registered before transfer dispatch in worker threads
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!PHP_ZTS) die('skip ZTS required');
if (!class_exists('Async\ThreadPool')) die('skip ThreadPool not available');
?>
--FILE--
<?php
/*
 * Regression for handler dispatch after cross-thread transfer_obj.
 *
 * 006-server-transfer-threadpool checks that the transferred object's
 * identity, isRunning() flag, and frozen config snapshot survive the
 * trip into a worker thread. It does *not* exercise the request path,
 * so a regression where the protocol_mask (consulted by
 * detect_and_assign_protocol) was lost during transfer slipped past:
 * worker threads bound the listen socket, parsed incoming bytes, but
 * silently dropped the request because the mask said "no HTTP/1
 * handler" even though one was loaded into protocol_handlers.
 *
 * This test sends a real HTTP/1.1 request to a server that runs
 * exclusively inside ThreadPool worker threads (no main-thread
 * start()), then asserts the handler ran and the registered response
 * came back.
 */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;

$port = 19840 + getmypid() % 50;

spawn(function () use ($port) {
    $config = (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5);

    $server = new HttpServer($config);
    $server->addHttpHandler(function ($req, $res) {
        $res->setStatusCode(200)
            ->setHeader('Content-Type', 'text/plain')
            ->setBody('handler-ran:' . $req->getUri());
    });

    $pool = new ThreadPool(2);
    for ($i = 0; $i < 2; $i++) {
        $pool->submit(function () use ($server) {
            $server->start();
        });
    }

    /* Give workers a beat to bind. */
    usleep(80000);

    /* Send two requests; with two workers they may land on either. */
    foreach (['/alpha', '/beta'] as $path) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        if (!$fp) {
            echo "connect failed: $errstr\n";
            continue;
        }
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        stream_set_timeout($fp, 2);
        $resp = '';
        while (!feof($fp)) {
            $c = fread($fp, 8192);
            if ($c === '' || $c === false) break;
            $resp .= $c;
        }
        fclose($fp);

        $statusLine = strtok($resp, "\r\n");
        $bodyStart  = strpos($resp, "\r\n\r\n");
        $body       = $bodyStart === false ? '' : substr($resp, $bodyStart + 4);
        echo "$path: $statusLine | $body\n";
    }

    /* Each worker has its own runtime; stop() on the main shell does
     * not propagate. Use the pool's cancel() to abort the blocking
     * start() calls so the test process exits. */
    $pool->cancel();
});

echo "Done\n";
?>
--EXPECT--
/alpha: HTTP/1.1 200 OK | handler-ran:/alpha
/beta: HTTP/1.1 200 OK | handler-ran:/beta
Done
