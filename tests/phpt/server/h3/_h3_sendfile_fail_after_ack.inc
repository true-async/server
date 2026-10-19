<?php
/* Shared body of h3/099 and h3/100: a sendFile() body whose second read fails
 * only after the peer has ACKed the first chunk. Fault point
 * h3/sendfile/read_failed_after_ack parks the failure on a timer, so the pump
 * fails from a timer callback, outside any UDP poll tick or mailbox drain, at a
 * moment when the peer owes no packet. A reset that waits for the next packet
 * from the peer misses the h3client's deadline.
 * Where the listener runs comes from TRUE_ASYNC_SERVER_REACTOR_POOL in --ENV--.
 *
 * Usage:
 *     require __DIR__ . '/_h3_sendfile_fail_after_ack.inc';
 *     h3_sendfile_fail_after_ack_run('099');
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require_once __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

const H3_FAIL_AFTER_ACK_POINT = 'h3/sendfile/read_failed_after_ack';
const H3_FAIL_AFTER_ACK_DEADLINE_MS = 3000;

function h3_sendfile_fail_after_ack_run(string $tag): void
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

    /* Several read chunks, so a second read follows the first one sent. */
    $fileBody = str_repeat('sendfile-0123456789', 8192);
    file_put_contents("$tmp/body.bin", $fileBody);

    $server = new HttpServer($config);
    $server->addHttpHandler(function ($req, $res) use ($tmp) {
        $res->sendFile("$tmp/body.bin");
    });

    $client = __DIR__ . '/../../../h3client/h3client';

    spawn(function () use ($server, $port, $client, $tmp, $fileBody) {
        delay(600);
        _http_fault_enable(H3_FAIL_AFTER_ACK_POINT, true);
        $out = (string) shell_exec('H3CLIENT_DEADLINE_MS=' . H3_FAIL_AFTER_ACK_DEADLINE_MS . ' '
            . escapeshellarg($client) . ' 127.0.0.1 ' . $port . ' /file GET 2>' . escapeshellarg("$tmp/stderr"));
        $err = (string) file_get_contents("$tmp/stderr");

        echo 'pump fault=', _http_fault_hits(H3_FAIL_AFTER_ACK_POINT), "\n";

        if (str_contains($err, 'RESET=') && !str_contains($err, 'timeout') && strlen($out) < strlen($fileBody)) {
            echo "peer: reset in time, body incomplete\n";
        } else {
            echo "peer: no reset before the deadline\nclient diagnostic: ", $err, "\n";
        }

        $server->stop();
    });

    $server->start();
}
