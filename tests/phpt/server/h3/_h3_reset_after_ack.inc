<?php
/* Shared body of h3/095-098: a handler writes one chunk, waits until the peer
 * has ACKed it, then ends the response with a stream reset. Once everything is
 * ACKed ngtcp2 owes no packet and its timer sits at the idle timeout, so the
 * peer sends nothing more; a reset that waits for the next packet from the peer
 * misses the in-tree h3client's deadline.
 *
 * Two ways to end the response:
 *  - 'abort': $res->abort(), and the handler stays parked past the deadline, so
 *    nothing at handler exit can send the reset in its place;
 *  - 'throw': the handler throws, and the reset goes out with the handler's
 *    disposal.
 * Where the listener runs comes from TRUE_ASYNC_SERVER_REACTOR_POOL in --ENV--.
 *
 * Usage:
 *     require __DIR__ . '/_h3_reset_after_ack.inc';
 *     h3_reset_after_ack_run('095', 'abort');
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require_once __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

/* Loopback ACKs the chunk in well under this. */
const H3_RESET_ACK_WAIT_MS = 300;
const H3_RESET_DEADLINE_MS = 3000;
/* Keeps an aborting handler alive past the client's deadline. */
const H3_RESET_PARK_MS = H3_RESET_DEADLINE_MS + 500;

function h3_reset_after_ack_run(string $tag, string $how): void
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
        ->setCertificate("$tmp/cert.pem")->setPrivateKey("$tmp/key.pem")
        ->setReadTimeout(10)->setWriteTimeout(10);

    if (getenv('TRUE_ASYNC_SERVER_REACTOR_POOL') === '1') {
        $config->setWorkers(2);
    }

    $server = new HttpServer($config);
    $server->addHttpHandler(function ($req, $res) use ($how) {
        $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
        $res->write(str_repeat('a', 4096));
        delay(H3_RESET_ACK_WAIT_MS);

        if ($how === 'throw') {
            throw new \RuntimeException('handler failed after the peer ACKed its body');
        }

        $res->abort();
        delay(H3_RESET_PARK_MS);
    });

    $client = __DIR__ . '/../../../h3client/h3client';

    spawn(function () use ($server, $port, $client, $tmp) {
        delay(600);
        shell_exec('H3CLIENT_DEADLINE_MS=' . H3_RESET_DEADLINE_MS . ' ' . escapeshellarg($client)
            . ' 127.0.0.1 ' . $port . ' /reset GET >/dev/null 2>' . escapeshellarg("$tmp/stderr"));
        $err = (string) file_get_contents("$tmp/stderr");

        if (str_contains($err, 'RESET=') && !str_contains($err, 'timeout')) {
            echo "peer: reset in time\n";
        } else {
            echo "peer: no reset before the deadline\nclient diagnostic: ", $err, "\n";
        }

        $server->stop();
    });

    $server->start();
}
