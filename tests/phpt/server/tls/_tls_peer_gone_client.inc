<?php
/* TLS client for 021: requests <path>, reads until the first chunk, then leaves —
 * "reset" with a RST (SO_LINGER {1,0}), anything else with close_notify and a FIN.
 * Runs as its own process so the handshake does not block the server's loop.
 *
 * php _tls_peer_gone_client.inc <port> <path> <reset|fin> */

[, $port, $path, $how] = $argv;

$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 4, STREAM_CLIENT_CONNECT, $ctx);
if (!$fp) {
    fwrite(STDERR, "connect failed: $errstr\n");
    exit(1);
}

// The linger is set on the plain socket: a TLS stream cannot be imported.
if ($how === 'reset') {
    socket_set_option(socket_import_stream($fp), SOL_SOCKET, SO_LINGER, ['l_onoff' => 1, 'l_linger' => 0]);
}

if (!stream_socket_enable_crypto($fp, true, STREAM_CRYPTO_METHOD_TLS_CLIENT)) {
    fwrite(STDERR, "handshake failed\n");
    exit(1);
}

fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\n\r\n");
$got = '';
while (!str_contains($got, 'first') && !feof($fp)) {
    $got .= fread($fp, 4096);
}

fclose($fp);
