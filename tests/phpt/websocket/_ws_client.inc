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

    if (!str_contains($hs, '101')) {
        return null;
    }

    /* A handler that sends the moment it is entered lands its first frames in the
     * same segment as the 101 — keep whatever followed the header, or the reads
     * below silently start at frame 3. */
    $rest = substr($hs, strpos($hs, "\r\n\r\n") + 4);

    if ($rest !== '') {
        ws_pushback($fp, $rest);
    }

    return $fp;
}

/* Bytes read past the handshake, waiting to be consumed as frames. */
function &ws_buffer($fp): string
{
    static $buffers = [];

    $key = (int) $fp;

    if (!isset($buffers[$key])) {
        $buffers[$key] = '';
    }

    return $buffers[$key];
}

function ws_pushback($fp, string $bytes): void
{
    $buf = &ws_buffer($fp);
    $buf .= $bytes;
}

/* Reads exactly $len bytes, buffer first, socket after. */
function ws_take($fp, int $len, bool $blocking): string
{
    $buf = &ws_buffer($fp);
    $out = '';

    if ($buf !== '') {
        $out = substr($buf, 0, $len);
        $buf = substr($buf, strlen($out));
    }

    while (strlen($out) < $len) {
        $chunk = fread($fp, $len - strlen($out));

        if ($chunk === false || $chunk === '') {
            break;
        }

        $out .= $chunk;

        if (!$blocking) {
            break;
        }
    }

    return $out;
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
    $hdr = ws_take($fp, 2, false);
    if (strlen($hdr) < 2) { return null; }

    $opcode = ord($hdr[0]) & 0x0f;
    $len    = ord($hdr[1]) & 0x7f;

    /* 126 and 127 escape to a 16- or 64-bit length. A server frame is never
     * masked, so the payload starts right after. */
    if ($len === 126) {
        $len = unpack('n', ws_take($fp, 2, true))[1];
    } elseif ($len === 127) {
        $len = unpack('J', ws_take($fp, 8, true))[1];
    }

    return ['opcode' => $opcode, 'data' => ws_take($fp, $len, true)];
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

/* Waits for one frame, up to $timeout_ms, and returns '' if none came.
 *
 * Use this instead of "delay(250); ws_read_pending()" for anything that MUST
 * arrive: a fixed sleep is tuned to an idle machine and turns into a flake the
 * moment the box is loaded — which is exactly when the whole suite runs. A
 * negative assertion ("this must NOT arrive") still needs a plain delay. */
function ws_await($fp, int $timeout_ms = 5000): string
{
    $deadline = microtime(true) + $timeout_ms / 1000;

    do {
        $frame = ws_read_pending($fp);

        if ($frame !== '') {
            return $frame;
        }

        \Async\delay(20);
    } while (microtime(true) < $deadline);

    return '';
}
