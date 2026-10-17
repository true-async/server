--TEST--
Rate limit (#414): HTTP/2 answers 429, gRPC gets grpc-status 8 on HTTP/2 and a plain 429 on HTTP/1, extended CONNECT is refused before the WebSocket handler
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\\WebSocket')) die('skip needs WebSocket support');
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* One request a minute under each of /h2, /svc.Greeter and /ws. The refused
 * h2c GET carries 429 and Retry-After; the refused gRPC call keeps its 200 and
 * ends Trailers-Only with grpc-status 8 (RESOURCE_EXHAUSTED) and a pushback
 * of 60000 ms, so it counts as a 2xx; the same call as gRPC-Web over HTTP/1
 * is a plain 429; and a refused RFC 8441 CONNECT never reaches the WebSocket
 * handler and ends its stream with the 429 body. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use TrueAsync\WebSocket;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/../h2/_h2_client.inc';
require_once __DIR__ . '/_rate_limit.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0));

foreach (['/h2', '/svc.Greeter', '/ws'] as $path) {
    $server->addRateLimit((new RateLimit(1, 60))->paths($path)->name(trim($path, '/')));
}

$runs = ['http' => 0, 'grpc' => 0, 'ws' => 0];
$server->addHttpHandler(function ($req, $res) use (&$runs) { $runs['http']++; $res->setBody('ok'); });
$server->addGrpcHandler(function ($req, $res) use (&$runs) {
    $runs['grpc']++;
    $req->awaitBody();
    $res->writeMessage('pong');
});
$server->addWebSocketHandler(function (WebSocket $ws) use (&$runs) {
    $runs['ws']++;
    while ($ws->recv() !== null) {}
});

/* The :status and the plain (not Huffman-coded) values of an HPACK block,
 * enough to read a short answer: nghttp2 sends "429" and "60" as plain
 * strings, as Huffman would not make them shorter. */
function hpack_status_and_values(string $block): array
{
    $int = function (int $prefix) use ($block, &$pos): int {
        $max = (1 << $prefix) - 1;
        $v = ord($block[$pos++]) & $max;
        if ($v < $max) return $v;
        for ($m = 0; ; $m += 7) {
            $b = ord($block[$pos++]);
            $v += ($b & 0x7f) << $m;
            if (!($b & 0x80)) return $v;
        }
    };
    $string = function () use ($block, &$pos, $int): ?string {
        $huffman = (ord($block[$pos]) & 0x80) !== 0;
        $len = $int(7);
        $raw = substr($block, $pos, $len);
        $pos += $len;
        return $huffman ? null : $raw;
    };

    $static_status = [8 => '200', 9 => '204', 10 => '206', 11 => '304', 12 => '400', 13 => '404', 14 => '500'];
    $status = null;
    $values = [];
    $pos = 0;

    while ($pos < strlen($block)) {
        $b = ord($block[$pos]);

        if ($b & 0x80) {                          /* indexed field */
            $index = $int(7);
            $status ??= $static_status[$index] ?? null;
            continue;
        }

        if (($b & 0xe0) === 0x20) {               /* table size update */
            $int(5);
            continue;
        }

        $index = ($b & 0x40) ? $int(6) : $int(4);
        if ($index === 0) {
            $string();                            /* literal name */
        }
        $value = $string();

        if ($index === 8 && $value !== null) {
            $status ??= $value;
        } elseif ($value !== null) {
            $values[] = $value;
        }
    }

    return [$status, $values];
}

/* The Retry-After among a refused CONNECT's plain values: the one made of
 * digits alone other than the Content-Length; the date has letters. */
function connect_retry_after(array $values, int $content_length): string
{
    foreach ($values as $value) {
        if (preg_match('/^\d+$/D', $value) === 1 && (int) $value !== $content_length) {
            return (string) rl_wait((int) $value);
        }
    }

    return 'missing';
}

function curl_lines(string $args, array $want): string
{
    $out = shell_exec("curl -s -i --max-time 3 $args 2>&1") ?? '';
    $found = [];

    foreach (preg_split('/\r?\n/', $out) as $line) {
        foreach ($want as $prefix) {
            if (stripos($line, $prefix) !== 0) {
                continue;
            }

            $line = strtolower(trim($line));

            if (preg_match('/^retry-after: (\d+)$/', $line, $m)) {
                $line = 'retry-after: ' . rl_wait((int) $m[1]);
            } elseif (preg_match('/^grpc-retry-pushback-ms: (\d+)$/', $line, $m)) {
                $line = 'grpc-retry-pushback-ms: ' . rl_wait(intdiv((int) $m[1], 1000)) * 1000;
            }

            $found[] = $line;
        }
    }

    return implode(' | ', $found);
}

$client = spawn(function () use ($port, $server, &$runs) {
    delay(50);
    $base = "http://127.0.0.1:$port";

    for ($i = 1; $i <= 2; $i++) {
        echo "h2c GET #$i: ", curl_lines("--http2-prior-knowledge $base/h2", ['HTTP/', 'retry-after']), "\n";
    }

    $frame = tempnam(sys_get_temp_dir(), 'rl096');
    file_put_contents($frame, "\x00" . pack('N', 4) . 'ping');
    /* escapeshellarg: cmd.exe takes single quotes literally. */
    $grpc = '--http2-prior-knowledge -H ' . escapeshellarg('content-type: application/grpc')
          . ' -H ' . escapeshellarg('te: trailers') . ' --data-binary ' . escapeshellarg("@$frame")
          . " $base/svc.Greeter/Ping";
    $want = ['HTTP/', 'grpc-status', 'grpc-message', 'grpc-retry-pushback-ms', 'retry-after'];

    echo "gRPC #1: ", curl_lines($grpc, $want), "\n";
    echo "gRPC #2: ", curl_lines($grpc, $want), "\n";
    $grpc_web = '--http1.1 -H ' . escapeshellarg('content-type: application/grpc-web+proto')
              . ' --data-binary ' . escapeshellarg("@$frame") . " $base/svc.Greeter/Ping";
    echo "gRPC-Web over HTTP/1: ", curl_lines($grpc_web, $want), "\n";
    @unlink($frame);

    /* Extended CONNECT: the first opens a WebSocket, the second is refused. */
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    foreach ([1, 2] as $i) {
        $sid = $cli->sendRequest('CONNECT', '/ws', 'localhost', [':protocol' => 'websocket'], null, false);
        $headers = false;
        $status = null;
        $values = [];
        $first_200 = false;
        $body = '';
        $ended = false;
        $deadline = microtime(true) + 3;

        while (microtime(true) < $deadline) {
            $f = $cli->readFrame();
            if ($f === null) break;
            [$type, $flags, $fsid, $payload] = $f;

            if ($type === H2_FRAME_SETTINGS && !($flags & H2_FLAG_ACK)) {
                $cli->sendSettingsAck();
                continue;
            }

            if ($fsid !== $sid) continue;

            if ($type === H2_FRAME_HEADERS) {
                $headers = true;
                /* 0x88 is the static-table index of :status 200. */
                $first_200 = ord($payload[0]) === 0x88;
                [$status, $values] = hpack_status_and_values($payload);
                $ended = (bool)($flags & H2_FLAG_END_STREAM);
                if ($i === 1 || $ended) break;
            }

            if ($type === H2_FRAME_DATA) {
                $body .= $payload;
                if ($flags & H2_FLAG_END_STREAM) { $ended = true; break; }
            }
        }

        echo "CONNECT #$i: headers=", $headers ? 'yes' : 'no', " status=", $status ?? '?',
             " indexed-200=", !empty($first_200) ? 'yes' : 'no',
             $i === 2 ? ' retry-after=' . connect_retry_after($values, strlen($body)) : '',
             " ended=", $ended ? 'yes' : 'no', " body=", trim($body), "\n";
    }

    $cli->close();
    delay(100);

    echo "handlers run: http={$runs['http']} grpc={$runs['grpc']} ws={$runs['ws']}\n";
    $t = $server->getTelemetry();
    echo "rate_limited_total=", $t['requests_rate_limited_total'], "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
h2c GET #1: http/2 200
h2c GET #2: http/2 429 | retry-after: 60
gRPC #1: http/2 200
gRPC #2: http/2 200 | grpc-status: 8 | grpc-message: rate limit exceeded | grpc-retry-pushback-ms: 60000
gRPC-Web over HTTP/1: http/1.1 429 too many requests | retry-after: 60
CONNECT #1: headers=yes status=200 indexed-200=yes ended=no body=
CONNECT #2: headers=yes status=429 indexed-200=no retry-after=60 ended=yes body=Too Many Requests
handlers run: http=1 grpc=1 ws=1
rate_limited_total=4
done
