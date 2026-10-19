<?php
/* Shared body of h3/101 and h3/102: responses that end normally — a plain
 * body, a streamed body and a sendFile() body — must cost no flush wake.
 * Their output is either drained in place or marked inside a poll tick or a
 * reactor mailbox drain, whose end flushes it without a syscall. A wake here
 * would put an eventfd write on every response.
 * Where the listener runs comes from TRUE_ASYNC_SERVER_REACTOR_POOL in --ENV--.
 *
 * Usage:
 *     require __DIR__ . '/_h3_flush_wakes.inc';
 *     h3_flush_wakes_run('101');
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require_once __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

function h3_flush_wakes_total(HttpServer $server): int
{
    $total = 0;

    foreach ($server->getHttp3Stats() as $listener) {
        $total += (int) ($listener['quic_flush_wakes'] ?? -1);
    }

    return $total;
}

function h3_flush_wakes_run(string $tag): void
{
    $tmp = __DIR__ . "/tmp-$tag-" . getmypid();
    mkdir($tmp, 0700);
    register_shutdown_function(function () use ($tmp) {
        foreach (glob("$tmp/*") as $f) {
            @unlink($f);
        }
        @rmdir($tmp);
    });

    if (!h3_gen_cert("$tmp/key.pem", "$tmp/cert.pem")) {
        echo "cert failed\n";
        return;
    }

    $port = tas_free_port_span(2);
    $config = (new HttpServerConfig())
        ->addListener('127.0.0.1', $port + 1)
        ->addHttp3Listener('127.0.0.1', $port)
        ->setCertificate("$tmp/cert.pem")->setPrivateKey("$tmp/key.pem");

    if (getenv('TRUE_ASYNC_SERVER_REACTOR_POOL') === '1') {
        $config->setWorkers(2);
    }

    /* Several read chunks of the static pump. */
    file_put_contents("$tmp/body.bin", str_repeat('sendfile-0123456789', 8192));

    $server = new HttpServer($config);
    $server->addHttpHandler(function ($req, $res) use ($tmp) {
        switch ($req->getUri()) {
            case '/plain':
                $res->setStatusCode(200)->setBody('plain');
                break;

            case '/stream':
                $res->setStatusCode(200);
                $res->write(str_repeat('a', 4096));
                delay(20);
                $res->write(str_repeat('b', 4096));
                $res->end();
                break;

            default:
                $res->sendFile("$tmp/body.bin");
        }
    });

    $client = __DIR__ . '/../../../h3client/h3client';

    spawn(function () use ($server, $port, $client) {
        delay(600);

        foreach (['/plain', '/stream', '/file'] as $path) {
            $out = (string) shell_exec(escapeshellarg($client) . ' 127.0.0.1 ' . $port . ' '
                . $path . ' GET 2>&1');
            $status = preg_match('/^STATUS=(\d+)$/m', $out, $m) ? $m[1] : 'none';
            echo "$path: status $status, flush wakes ", h3_flush_wakes_total($server), "\n";
        }

        /* Stream-close teardown after the last client has gone. */
        delay(100);
        echo "after teardown: flush wakes ", h3_flush_wakes_total($server), "\n";

        $server->stop();
    });

    $server->start();
}
