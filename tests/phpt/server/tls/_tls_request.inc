<?php
/**
 * Minimal TLS HTTP client for phpt tests.
 * Runs as a separate process so the TLS handshake does not block the
 * server's async event loop (both sides live in the same PHP process
 * when using stream_socket_client inside a fiber).
 *
 * Usage:
 *   php _tls_request.php <host> <port> <method> <uri_repeat_char> <uri_repeat_n>
 *
 * Prints the raw HTTP response to stdout.
 * Extra flags (optional, appended after positional args):
 *   --timeout=N   connect+read timeout in seconds (default 4)
 */

$host    = $argv[1] ?? '127.0.0.1';
$port    = (int)($argv[2] ?? 0);
$method  = $argv[3] ?? 'GET';
$char    = $argv[4] ?? 'A';
$repeat  = (int)($argv[5] ?? 0);
$timeout = 4;
foreach ($argv as $a) {
    if (str_starts_with($a, '--timeout=')) {
        $timeout = (int)substr($a, 10);
    }
}

$uri = '/' . str_repeat($char, $repeat);

$ctx = stream_context_create(['ssl' => [
    'verify_peer'      => false,
    'verify_peer_name' => false,
]]);
$fp = @stream_socket_client("ssl://$host:$port", $errno, $errstr, $timeout,
                             STREAM_CLIENT_CONNECT, $ctx);
if (!$fp) {
    fwrite(STDERR, "connect failed ($errno): $errstr\n");
    exit(1);
}

fwrite($fp, "$method $uri HTTP/1.1\r\nHost: $host:$port\r\nConnection: close\r\n\r\n");
stream_set_timeout($fp, $timeout);
echo stream_get_contents($fp);
fclose($fp);
