--TEST--
HttpServer access log: an aborted response keeps the status it sent and says the body stopped short
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* #171 gave the peer the truth — a withheld terminator, a reset — and left the
 * record saying the response completed. Substituting the status would make the
 * record disagree with the wire, so it stays what the peer was told and the
 * abort goes in `error.type`, which OTel defines for a request that ends in an
 * error and forbids on one that did not. nginx's $request_completion, Apache's
 * %X and Envoy's RESPONSE_FLAGS all separate the two the same way.
 *
 * `http.response.body.size` is the second column read here: it counted the
 * buffered body, which a stream never fills and a 204 never sends.
 *
 * The counted abort is the one the transport could act on. An abort() that
 * reaches a stream with nothing on the wire is finished as an empty response
 * the peer receives whole, so it is a completed request and the record says
 * so — the flag the log reads is what the peer saw, not which call ended the
 * handler. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$path = sys_get_temp_dir() . '/php-http-066-access-' . getmypid() . '.log';
@unlink($path);
$fh = fopen($path, 'w+b');

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
        ->setStatsEnabled(true)
        ->setLogSinks([
            ['type' => 'stream', 'stream' => $fh, 'format' => 'json',
             'category' => 'access', 'level' => LogSeverity::INFO],
        ])
);

$server->addHttpHandler(function ($req, $res) {
    switch ($req->getPath()) {
        case '/aborted':
            $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
            $res->write('alpha');
            $res->abort();
            return;

        case '/sse-no-event':
            /* abort() on a stream the transport never put a byte of: the peer
             * gets a whole empty response, so the record must not say the body
             * stopped short. */
            $res->sseStart();
            $res->abort();
            return;

        case '/streamed':
            $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
            $res->write('alpha');
            $res->end('beta');
            return;

        case '/nocontent':
            $res->setStatusCode(204)->setBody('oops')->end();
            return;
    }

    $res->setBody('hello')->end();
});

spawn(function () use ($server, $port) {
    usleep(50000);

    foreach (['/aborted', '/sse-no-event', '/streamed', '/nocontent'] as $target) {
        $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);

        if (!$c) {
            continue;
        }

        stream_set_timeout($c, 2);
        fwrite($c, "GET $target HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        while (!feof($c)) {
            if (@fread($c, 8192) === false) {
                break;
            }
        }

        fclose($c);
    }

    $aborted = 0;
    for ($p = 0; $p < 50; $p++) {
        $totals = $server->getStats()['totals'];

        if (($totals['total_requests'] ?? 0) >= 4) {
            $aborted = $totals['responses_aborted_total'] ?? -1;
            break;
        }

        usleep(20000);
    }

    echo "responses_aborted_total=$aborted\n";
    $server->stop();
});

$server->start();

fflush($fh);
fclose($fh);
$lines = trim((string) file_get_contents($path));
@unlink($path);

$by_path = [];
foreach (explode("\n", $lines) as $line) {
    if ($line === '') {
        continue;
    }

    $attrs = json_decode($line, true)['Attributes'] ?? [];
    $by_path[$attrs['url.path'] ?? '?'] = $attrs;
}

foreach (['/aborted', '/sse-no-event', '/streamed', '/nocontent'] as $target) {
    $a = $by_path[$target] ?? null;
    echo $target, ': ', $a === null ? 'missing' : sprintf(
        'status=%d bytes=%d error=%s',
        $a['http.response.status_code'],
        $a['http.response.body.size'],
        $a['error.type'] ?? '-'), "\n";
}

echo "Done\n";
?>
--EXPECT--
responses_aborted_total=1
/aborted: status=200 bytes=5 error=response_aborted
/sse-no-event: status=200 bytes=0 error=-
/streamed: status=200 bytes=9 error=-
/nocontent: status=204 bytes=0 error=-
Done
