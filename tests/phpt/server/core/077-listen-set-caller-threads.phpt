--TEST--
HttpServer: caller-spawned threads share one listening socket (#275)
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
 * The scenario #275 is about: the caller spawns the threads itself instead
 * of asking for workers in the config, and every thread calls start() on one
 * transferred server.
 *
 * Before the shared listen set, only the built-in pool filled the pre-bound
 * fd table, so each of these threads bound a socket of its own. On a kernel
 * without load-balanced SO_REUSEPORT the second bind lost, and on Windows —
 * which has no SO_REUSEPORT at all and had no sharing either — every thread
 * past the first lost. The losing thread served nothing, silently until
 * #273 and with "address already in use" after it.
 *
 * Four requests are enough: the point is that every one of them is answered,
 * not which thread answered it. Accept distribution across a shared queue is
 * the kernel's business and deliberately not asserted here.
 */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

const WORKERS  = 2;
const REQUESTS = 4;

$port = tas_free_port();

$coro = spawn(function () use ($port) {
    $config = (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5);

    $server = new HttpServer($config);
    $server->addHttpHandler(function ($req, $res) {
        $res->setStatusCode(200)
            ->setHeader('Content-Type', 'text/plain')
            ->setBody('served');
    });

    $pool = new ThreadPool(WORKERS);
    $futures = [];

    for ($i = 0; $i < WORKERS; $i++) {
        $futures[] = $pool->submit(function () use ($server) {
            /* stop() from the main thread does not reach a worker's own
             * runtime, so each worker schedules its own shutdown. */
            spawn(function () use ($server) {
                usleep(2500000);
                $server->stop();
            });
            $server->start();
        });
    }

    /* Wait for the listener to answer before measuring anything. */
    $up = false;
    $deadline = microtime(true) + 3.0;
    while (microtime(true) < $deadline) {
        $probe = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($probe) { fclose($probe); $up = true; break; }
        usleep(20000);
    }

    echo 'listening: ', $up ? 'yes' : 'no', "\n";

    $answered = 0;
    for ($i = 0; $i < REQUESTS; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        if (!$fp) {
            continue;
        }

        fwrite($fp, "GET /$i HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $resp = '';
        while (!feof($fp)) {
            $chunk = fread($fp, 4096);
            if ($chunk === '' || $chunk === false) break;
            $resp .= $chunk;
        }
        fclose($fp);

        if (str_contains($resp, '200 OK') && str_contains($resp, 'served')) {
            $answered++;
        }
    }

    echo "answered: $answered of ", REQUESTS, "\n";

    foreach ($futures as $f) {
        await($f);
    }
    $pool->close();
});

await($coro);
echo "Done\n";
?>
--EXPECT--
listening: yes
answered: 4 of 4
Done
