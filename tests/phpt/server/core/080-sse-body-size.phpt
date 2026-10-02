--TEST--
HttpServer access log: an SSE response is logged with the body octets it sent
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* The SSE dialect hands its records to the transport directly rather than
 * through write(), and every record, comment and non-blocking offer still
 * counts toward http.response.body.size. Checked on HTTP/1 and HTTP/2 against
 * the body the client decoded, with end() carrying the last record. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/../h2/_h2_skipif.inc';

$log = sys_get_temp_dir() . '/php-http-080-access-' . getmypid() . '.log';
@unlink($log);
$fh = fopen($log, 'w+b');
$h2_body = sys_get_temp_dir() . '/php-http-080-h2-' . getmypid() . '.txt';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
        ->setLogSinks([
            ['type' => 'stream', 'stream' => $fh, 'format' => 'json',
             'category' => 'access', 'level' => LogSeverity::INFO],
        ])
);

$server->addHttpHandler(function ($req, $res) {
    $res->sseEvent("hello");
    $res->sseEvent("named", event: "ping", id: "7");
    $res->sseComment();
    $res->trySseEvent("try");
    $res->end("data: bye\n\n");
});

function dechunk(string $wire): string
{
    $body = '';
    while (preg_match('/^([0-9a-fA-F]+)\r\n/', $wire, $m)) {
        $len = hexdec($m[1]);
        $body .= substr($wire, strlen($m[0]), $len);
        $wire = substr($wire, strlen($m[0]) + $len + 2);
        if ($len === 0) { break; }
    }

    return $body;
}

/* The access record is written once the response is done, which the client
 * cannot see; the file is read until both records are in it. */
function access_sizes(string $log): array
{
    $size = [];
    foreach (explode("\n", trim((string) file_get_contents($log))) as $line) {
        $attrs = json_decode($line, true)['Attributes'] ?? [];
        if (isset($attrs['url.path'])) {
            $size[$attrs['url.path']] = $attrs['http.response.body.size'] ?? -1;
        }
    }

    return $size;
}

$client = spawn(function () use ($server, $port, $log, $h2_body) {
    $received = [];

    $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
    if ($c) {
        stream_set_timeout($c, 3);
        fwrite($c, "GET /h1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $raw = '';
        while (!feof($c)) {
            $chunk = @fread($c, 8192);
            if ($chunk === false || $chunk === '') { break; }
            $raw .= $chunk;
        }
        fclose($c);
        [, $chunked] = explode("\r\n\r\n", $raw, 2) + [1 => ''];
        $received['/h1'] = strlen(dechunk($chunked));
    }

    h2_curl(['--http2-prior-knowledge', '-s', '-o', $h2_body, '--max-time', '3',
        "http://127.0.0.1:$port/h2"]);
    $received['/h2'] = strlen((string) @file_get_contents($h2_body));

    for ($waited = 0; count(access_sizes($log)) < 2 && $waited < 3000; $waited += 20) {
        delay(20);
    }

    $server->stop();
    return $received;
});

$server->start();
$received = Async\await($client);

fclose($fh);
$size = access_sizes($log);
@unlink($log);
@unlink($h2_body);

foreach (['/h1', '/h2'] as $path) {
    echo "$path received: ", $received[$path] ?? 'missing', "\n";
    echo "$path logged:   ", $size[$path] ?? 'missing', "\n";
}
?>
--EXPECT--
/h1 received: 69
/h1 logged:   69
/h2 received: 69
/h2 logged:   69
