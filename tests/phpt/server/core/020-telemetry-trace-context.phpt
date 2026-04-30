--TEST--
HttpServer: telemetry parses W3C traceparent / tracestate (PLAN_LOG.md Step 5)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19770 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setTelemetryEnabled(true);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $payload = json_encode([
        'tp'      => $req->getTraceParent(),
        'ts'      => $req->getTraceState(),
        'tid'     => $req->getTraceId(),
        'sid'     => $req->getSpanId(),
        'flags'   => $req->getTraceFlags(),
    ]);
    $res->setStatusCode(200)->setBody($payload)->end();
});

function fetch(int $port, array $extra_headers): string {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) return '';
    $head = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n";
    foreach ($extra_headers as $h) { $head .= $h . "\r\n"; }
    $head .= "\r\n";
    fwrite($fp, $head);
    stream_set_timeout($fp, 2);
    $resp = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $resp .= $c;
    }
    fclose($fp);
    $i = strpos($resp, "\r\n\r\n");
    return $i === false ? '' : substr($resp, $i + 4);
}

$client = spawn(function () use ($port, $server) {
    usleep(40000);

    /* 1) Valid traceparent + tracestate. */
    $tp = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
    $ts = "vendor1=value1,vendor2=value2";
    $body = fetch($port, ["traceparent: $tp", "tracestate: $ts"]);
    echo "valid: ", $body, "\n";

    /* 2) Malformed (uppercase hex — W3C requires lowercase). */
    $bad = "00-0AF7651916CD43DD8448EB211C80319C-B7AD6B7169203331-01";
    $body = fetch($port, ["traceparent: $bad"]);
    echo "upper: ", $body, "\n";

    /* 3) All-zero trace_id sentinel — must be rejected. */
    $zero = "00-00000000000000000000000000000000-b7ad6b7169203331-01";
    $body = fetch($port, ["traceparent: $zero"]);
    echo "zero-tid: ", $body, "\n";

    /* 4) No headers — getters return null. */
    $body = fetch($port, []);
    echo "absent: ", $body, "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
valid: {"tp":"00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01","ts":"vendor1=value1,vendor2=value2","tid":"0af7651916cd43dd8448eb211c80319c","sid":"b7ad6b7169203331","flags":1}
upper: {"tp":null,"ts":null,"tid":null,"sid":null,"flags":null}
zero-tid: {"tp":null,"ts":null,"tid":null,"sid":null,"flags":null}
absent: {"tp":null,"ts":null,"tid":null,"sid":null,"flags":null}
Done
