<?php
/* Minimal RFC 6455 client for the room tests: open, send a masked frame, read
 * one frame back. Enough to drive a server, not a general-purpose client. */

function ws_open(int $port, string $path = '/')
{
    $fp = false;
    for ($try = 0; $try < 50 && $fp === false; $try++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
        if ($fp === false) { usleep(200000); }
    }
    if ($fp === false) { return null; }

    stream_set_timeout($fp, 3);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') break;
        $hs .= $c;
    }

    return str_contains($hs, '101') ? $fp : null;
}

function ws_write($fp, string $payload, bool $binary = false): void
{
    $mask = random_bytes(4);
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }

    fwrite($fp, chr($binary ? 0x82 : 0x81) . chr(0x80 | strlen($payload)) . $mask . $masked);
}

/* One frame, or null when nothing is pending. Payloads here are always short. */
function ws_read_frame($fp): ?array
{
    $hdr = fread($fp, 2);
    if ($hdr === false || strlen($hdr) < 2) { return null; }

    $opcode = ord($hdr[0]) & 0x0f;
    $len    = ord($hdr[1]) & 0x7f;

    $data = '';
    while (strlen($data) < $len) {
        $chunk = fread($fp, $len - strlen($data));
        if ($chunk === false || $chunk === '') break;
        $data .= $chunk;
    }

    return ['opcode' => $opcode, 'data' => $data];
}

function ws_read($fp): string
{
    $frame = ws_read_frame($fp);

    return $frame !== null ? $frame['data'] : '';
}

/* Drain whatever the peer has already sent, without blocking. */
function ws_read_pending($fp): string
{
    stream_set_blocking($fp, false);
    $data = ws_read($fp);
    stream_set_blocking($fp, true);

    return $data;
}
