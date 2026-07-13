--TEST--
getStats (#5): the reported key set is the whole counter table; streaming counters and gauges move
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Contract test. Two things drift silently otherwise: a counter added to
 * http_server_counters_t but not to HTTP_SERVER_COUNTER_TABLE (so getStats
 * never reports it), and a key renamed under an exporter that keys off it.
 * Every counter the table declares must show up in `totals`, in every worker
 * slot, and nothing else may. `reactors` is listed even with no reactor: a
 * consumer summing workers + reactors must find both keys. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$expected = [
    'streaming_responses_total', 'stream_send_calls_total', 'stream_bytes_sent_total',
    'stream_send_backpressure_events_total', 'worker_wire_dropped_total',
    'h2_streams_active', 'h2_streams_opened_total', 'h2_streams_reset_by_peer_total',
    'h2_streams_refused_total', 'h2_goaway_recv_total', 'h2_goaway_sent_total',
    'h2_data_recv_bytes_total', 'h2_data_sent_bytes_total', 'h2_ping_rtt_ns',
    'h1_connection_close_sent_total', 'h3_goaway_sent_total',
    'requests_shed_total', 'active_requests',
    'tls_bytes_plaintext_in_total', 'tls_bytes_plaintext_out_total',
    'tls_bytes_ciphertext_in_total', 'tls_bytes_ciphertext_out_total',
    'total_requests',
    'responses_2xx_total', 'responses_3xx_total', 'responses_4xx_total', 'responses_5xx_total',
    'conns_active_h1', 'conns_active_h2', 'conns_active_h3',
    'static_zero_coroutine_total', 'static_cache_hits_total', 'static_cache_misses_total',
];

$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setStatsEnabled(true);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    if ($req->getPath() === '/stream') {
        $res->setStatusCode(200);
        $res->send('abc');
        $res->send('de');
        $res->end();
        return;
    }

    $res->setStatusCode($req->getPath() === '/gone' ? 404 : 200)->setBody('x')->end();
});

$get = function (string $path) use ($port) {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    while (!feof($fp)) { if (@fread($fp, 8192) === false) break; }
    fclose($fp);
};

spawn(function () use ($server, $get, $expected) {
    usleep(50000);

    $get('/ok');
    $get('/gone');
    $get('/stream');

    $stats = [];
    for ($i = 0; $i < 50; $i++) {
        $stats = $server->getStats();
        if (($stats['totals']['total_requests'] ?? 0) >= 3
            && ($stats['totals']['conns_active_h1'] ?? 1) === 0) {
            break;
        }
        usleep(20000);
    }

    $t = $stats['totals'];

    /* Shape: a consumer walks workers and reactors and sums into totals. */
    echo 'has_workers=',  (isset($stats['workers']) && is_array($stats['workers']) ? 1 : 0), "\n";
    echo 'has_reactors=', (isset($stats['reactors']) && is_array($stats['reactors']) ? 1 : 0), "\n";
    echo 'enabled=',      ($stats['enabled'] === true ? 1 : 0), "\n";

    /* Key set: exactly the counter table, in totals and in every worker slot. */
    $missing = array_values(array_diff($expected, array_keys($t)));
    $extra   = array_values(array_diff(array_keys($t), $expected));
    echo 'missing_in_totals=', $missing ? implode(',', $missing) : 'none', "\n";
    echo 'unexpected_in_totals=', $extra ? implode(',', $extra) : 'none', "\n";

    $worker_ok = 1;
    foreach ($stats['workers'] as $w) {
        if (array_diff($expected, array_keys($w)) !== []) { $worker_ok = 0; }
    }
    echo 'worker_slots_complete=', $worker_ok, "\n";

    /* Values: the three requests classify, and the streaming path is counted. */
    $classes = $t['responses_2xx_total'] + $t['responses_3xx_total']
             + $t['responses_4xx_total'] + $t['responses_5xx_total'];

    echo 'total=',      ($t['total_requests'] === 3 ? 1 : 0), "\n";
    echo 'class_sum=',  ($classes === $t['total_requests'] ? 1 : 0), "\n";
    echo '2xx=',        ($t['responses_2xx_total'] === 2 ? 1 : 0), "\n";
    echo '4xx=',        ($t['responses_4xx_total'] === 1 ? 1 : 0), "\n";
    echo 'streamed=',   ($t['streaming_responses_total'] === 1 ? 1 : 0), "\n";
    echo 'send_calls=', ($t['stream_send_calls_total'] >= 2 ? 1 : 0), "\n";
    echo 'send_bytes=', ($t['stream_bytes_sent_total'] >= 5 ? 1 : 0), "\n";

    /* Gauges are occupancy, not history: everything closed, so back to zero. */
    echo 'h1_drained=',  ($t['conns_active_h1'] === 0 ? 1 : 0), "\n";
    echo 'no_inflight=', ($t['active_requests'] === 0 ? 1 : 0), "\n";

    echo "done\n";
    $server->stop();
});

$server->start();
?>
--EXPECT--
has_workers=1
has_reactors=1
enabled=1
missing_in_totals=none
unexpected_in_totals=none
worker_slots_complete=1
total=1
class_sum=1
2xx=1
4xx=1
streamed=1
send_calls=1
send_bytes=1
h1_drained=1
no_inflight=1
done
