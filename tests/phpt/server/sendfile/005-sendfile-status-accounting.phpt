--TEST--
sendFile() accounting (#5): ranged send is counted/logged as 206, a failed send as 500 (h1)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The handler only queues the send and returns with the default 200 — the real
 * wire status is stamped later by the send-file engine. Counting and access-
 * logging therefore happen at the protocol teardown (http_request_account), so
 * a ranged send must report 206, not 200. A missing path is INLINE_500 by
 * design for a PHP-issued sendFile (only a static mount turns it into a 404). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$tmp = tempnam(sys_get_temp_dir(), 'sf-acct-');
file_put_contents($tmp, "0123456789");
$logPath = sys_get_temp_dir() . '/php-http-sf-acct-' . getmypid() . '.log';
@unlink($logPath);
$logFh = fopen($logPath, 'w+b');
register_shutdown_function(function () use ($tmp, $logPath) {
    @unlink($tmp);
    @unlink($logPath);
});

$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setStatsEnabled(true)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $logFh, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($tmp) {
    $res->sendFile($req->getPath() === '/missing' ? $tmp . '.nope' : $tmp);
});

$request = function (string $path, ?string $range) use ($port) {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
    stream_set_timeout($fp, 2);
    $hdr = $range !== null ? "Range: bytes=$range\r\n" : '';
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\n{$hdr}Connection: close\r\n\r\n");
    $resp = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $resp .= $c;
    }
    fclose($fp);
    return strtok($resp, "\r\n");
};

spawn(function () use ($server, $port, $request) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }

    echo "wire ranged:  ", $request('/ranged', '0-3'), "\n";
    echo "wire full:    ", $request('/full', null), "\n";
    echo "wire missing: ", $request('/missing', null), "\n";

    $t = [];
    for ($p = 0; $p < 50; $p++) {
        $t = $server->getStats()['totals'];
        if (($t['total_requests'] ?? 0) >= 3) break;
        usleep(20000);
    }

    $classes = $t['responses_2xx_total'] + $t['responses_3xx_total']
             + $t['responses_4xx_total'] + $t['responses_5xx_total'];

    echo 'total=',     ($t['total_requests'] === 3 ? 1 : 0), "\n";
    echo 'class_sum=', ($classes === $t['total_requests'] ? 1 : 0), "\n";
    echo '2xx=',       ($t['responses_2xx_total'] === 2 ? 1 : 0), "\n";
    echo '5xx=',       ($t['responses_5xx_total'] === 1 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();

/* Records only reach the fd once the sink ring has drained — read after stop. */
fclose($logFh);
$seen = [];
foreach (explode("\n", trim(file_get_contents($logPath))) as $line) {
    if ($line === '') continue;
    $a = json_decode($line, true)['Attributes'] ?? [];
    $seen[$a['url.path'] ?? '?'] = $a['http.response.status_code'] ?? '?';
}
echo 'log lines=', count($seen), "\n";
foreach (['/ranged', '/full', '/missing'] as $p) {
    echo "log $p: ", $seen[$p] ?? 'missing', "\n";
}
echo "done\n";
?>
--EXPECT--
wire ranged:  HTTP/1.1 206 Partial Content
wire full:    HTTP/1.1 200 OK
wire missing: HTTP/1.1 500 Internal Server Error
total=1
class_sum=1
2xx=1
5xx=1
log lines=3
log /ranged: 206
log /full: 200
log /missing: 500
done
