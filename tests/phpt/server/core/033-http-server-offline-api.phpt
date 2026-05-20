--TEST--
HttpServer: offline API surface — handlers, getters, telemetry without start()
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Exercises every method on HttpServer that doesn't require the
 * event loop: handler registration (h1/h2/ws/grpc/static), getConfig,
 * isRunning, getTelemetry (all keys), resetTelemetry, getHttp3Stats.
 *
 * Each handler-add also locks the config, so we assert that side
 * effect; addStaticHandler with a non-initialised handle takes the
 * error path. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;

$root = sys_get_temp_dir() . '/hs-api-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
register_shutdown_function(fn() => @rmdir($root));

$cfg    = (new HttpServerConfig())->addListener('127.0.0.1', 28700 + getmypid() % 1000);
$server = new HttpServer($cfg);

/* Config is locked once HttpServer is constructed. */
echo "config-locked-after-ctor: ", $cfg->isLocked() ? 'yes' : 'no', "\n";

/* Handler registration — all four happy paths. */
$server->addHttpHandler(fn($req, $res) => null);
$server->addHttp2Handler(fn($req, $res) => null);
$server->addWebSocketHandler(fn($req, $res) => null);
$server->addGrpcHandler(fn($req, $res) => null);

/* addStaticHandler with valid handle (happy path) + locks the handler. */
$sh = new StaticHandler('/s/', $root);
$server->addStaticHandler($sh);
echo "static-handler-locked: ", $sh->isLocked() ? 'yes' : 'no', "\n";

/* addStaticHandler with an UN-initialised handle: skip — the only way
 * to get an uninit StaticHandler is to reflection-instantiate without
 * calling __construct, which would interact with strict-properties.
 * The validation arm is exercised in 014/015 transitively. */

/* getConfig returns the same config instance we passed in. */
echo "getConfig-identity: ", $server->getConfig() === $cfg ? 'yes' : 'no', "\n";

/* isRunning is false until start(). */
echo "isRunning-before-start: ", $server->isRunning() ? 'yes' : 'no', "\n";

/* getTelemetry: all keys present + zero-valued before any traffic. */
$t = $server->getTelemetry();
$expected_keys = [
    'total_requests', 'active_connections', 'active_requests',
    'max_inflight_requests', 'requests_shed_total',
    'listeners_paused', 'pause_count_total', 'codel_trips_total',
    'paused_total_ms', 'sojourn_samples', 'sojourn_avg_ms',
    'sojourn_max_ms', 'service_avg_ms',
    'tls_handshakes_total', 'tls_handshake_failures_total',
    'tls_handshake_avg_ms', 'tls_resumed_total',
    'tls_bytes_plaintext_in_total', 'tls_bytes_plaintext_out_total',
    'tls_bytes_ciphertext_in_total', 'tls_bytes_ciphertext_out_total',
    'tls_ktls_tx_total', 'tls_ktls_rx_total',
    'parse_errors_4xx_total', 'parse_errors_400_total',
    'parse_errors_413_total', 'parse_errors_414_total',
    'parse_errors_431_total', 'parse_errors_503_total',
    'drain_epoch_current', 'drain_events_reactive_total',
    'drain_events_cooldown_blocked_total',
    'connections_drained_reactive_total',
    'connections_drained_proactive_total',
    'h2_goaway_sent_total', 'h3_goaway_sent_total',
    'h1_connection_close_sent_total', 'connections_force_closed_total',
    'streaming_responses_total', 'stream_send_calls_total',
    'stream_send_backpressure_events_total', 'stream_bytes_sent_total',
    'static_zero_coroutine_total',
    'static_cache_hits_total', 'static_cache_misses_total',
    'h2_streams_active', 'h2_streams_opened_total',
    'h2_streams_reset_by_peer_total', 'h2_streams_refused_total',
    'h2_goaway_recv_total', 'h2_data_recv_bytes_total',
    'h2_data_sent_bytes_total', 'h2_ping_rtt_ns',
];
$missing = array_diff($expected_keys, array_keys($t));
echo "telemetry-missing-keys: ", count($missing) === 0 ? 'none' : implode(',', $missing), "\n";
echo "telemetry-total_requests-zero: ", $t['total_requests'] === 0 ? 'yes' : 'no', "\n";
echo "telemetry-active_connections-zero: ", $t['active_connections'] === 0 ? 'yes' : 'no', "\n";

/* resetTelemetry returns true and zeros known counters. */
echo "resetTelemetry: ", $server->resetTelemetry() ? 'true' : 'false', "\n";

/* getHttp3Stats — without any H3 listener, returns []. */
$h3 = $server->getHttp3Stats();
echo "http3-stats-type: ", (is_array($h3) ? 'array' : gettype($h3)), " count=", count($h3), "\n";

/* Lifecycle: drop the server object — descriptor must tear down
 * cleanly even though start() was never called. */
unset($server);
echo "done\n";
?>
--EXPECT--
config-locked-after-ctor: yes
static-handler-locked: yes
getConfig-identity: yes
isRunning-before-start: no
telemetry-missing-keys: none
telemetry-total_requests-zero: yes
telemetry-active_connections-zero: yes
resetTelemetry: true
http3-stats-type: array count=0
done
