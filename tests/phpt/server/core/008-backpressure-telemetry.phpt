--TEST--
HttpServer: backpressure telemetry is populated by real traffic
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19700 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setBackpressureTargetMs(50);  // generous, we don't want a trip here

$server = new HttpServer($config);

$server->addHttpHandler(function($request, $response) {
    $response->setStatusCode(200)->setBody('OK');
});

// Client does N requests back-to-back on keep-alive, then stops server.
$clientCoroutine = spawn(function() use ($port, $server) {
    usleep(30000);  // let server come up

    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) { echo "connect failed: $errstr\n"; return; }
    stream_set_timeout($fp, 2);

    $n_ok = 0;
    for ($i = 0; $i < 5; $i++) {
        $req = "GET /$i HTTP/1.1\r\nHost: x\r\n\r\n";
        fwrite($fp, $req);

        // Read one full response (Content-Length: 2, body "OK")
        $data = '';
        while (!str_contains($data, "\r\n\r\n")) {
            $chunk = fread($fp, 1024);
            if ($chunk === false || $chunk === '') break;
            $data .= $chunk;
        }
        // Grab body
        [$hdr, $body] = explode("\r\n\r\n", $data, 2);
        while (strlen($body) < 2) {
            $chunk = fread($fp, 1024);
            if ($chunk === false || $chunk === '') break;
            $body .= $chunk;
        }
        if (str_contains($data, '200 OK') && $body === 'OK') {
            $n_ok++;
        }
    }
    fclose($fp);
    echo "client OK responses: $n_ok\n";

    $t = $server->getTelemetry();
    // Shape + sanity checks — exact values are timing-dependent.
    echo "listeners_paused is bool: ", (is_bool($t['listeners_paused']) ? 'yes' : 'no'), "\n";
    echo "total_requests >= n_ok: ", ($t['total_requests'] >= $n_ok ? 'yes' : 'no'), "\n";
    echo "sojourn_samples >= n_ok: ", ($t['sojourn_samples'] >= $n_ok ? 'yes' : 'no'), "\n";
    echo "sojourn_avg_ms is num: ", (is_float($t['sojourn_avg_ms']) ? 'yes' : 'no'), "\n";
    echo "sojourn_max_ms >= 0: ", ($t['sojourn_max_ms'] >= 0 ? 'yes' : 'no'), "\n";
    echo "service_avg_ms is num: ", (is_float($t['service_avg_ms']) ? 'yes' : 'no'), "\n";
    echo "codel_trips_total is int: ", (is_int($t['codel_trips_total']) ? 'yes' : 'no'), "\n";
    echo "pause_high=0 (no cap): ", ($t['pause_high'] === 0 ? 'yes' : 'no'), "\n";
    echo "not paused at end: ", ($t['listeners_paused'] === false ? 'yes' : 'no'), "\n";

    // resetTelemetry should zero the request counters.
    $server->resetTelemetry();
    $t2 = $server->getTelemetry();
    echo "after reset, total_requests = 0: ", ($t2['total_requests'] === 0 ? 'yes' : 'no'), "\n";
    echo "after reset, sojourn_samples = 0: ", ($t2['sojourn_samples'] === 0 ? 'yes' : 'no'), "\n";
    echo "after reset, sojourn_avg_ms = 0: ", ($t2['sojourn_avg_ms'] === 0.0 ? 'yes' : 'no'), "\n";

    $server->stop();
});

$server->start();
await($clientCoroutine);
echo "Done\n";
--EXPECT--
client OK responses: 5
listeners_paused is bool: yes
total_requests >= n_ok: yes
sojourn_samples >= n_ok: yes
sojourn_avg_ms is num: yes
sojourn_max_ms >= 0: yes
service_avg_ms is num: yes
codel_trips_total is int: yes
pause_high=0 (no cap): yes
not paused at end: yes
after reset, total_requests = 0: yes
after reset, sojourn_samples = 0: yes
after reset, sojourn_avg_ms = 0: yes
Done
