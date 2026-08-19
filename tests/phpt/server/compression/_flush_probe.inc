<?php
/* Probe for mid-stream delivery of a compressed response (#170).
 *
 * The handler under test emits one chunk and then parks until a request
 * to /release opens its gate, so everything the client has read before
 * that request is exactly what the encoder let through mid-stream. A
 * codec that only flushes at end of stream yields an empty prefix. */

/* Split an H1 response into headers and the de-chunked body. A trailing
 * partial chunk is left out: the prefix read so far is what matters. */
function tas_h1_split(string $raw): array
{
    $sep = strpos($raw, "\r\n\r\n");

    if ($sep === false) return [[], ''];
    $lines = explode("\r\n", substr($raw, 0, $sep));
    array_shift($lines);
    $h = [];

    foreach ($lines as $l) {
        if (strpos($l, ':') === false) continue;
        [$k, $v] = explode(':', $l, 2);
        $h[strtolower(trim($k))] = trim($v);
    }

    $rest = substr($raw, $sep + 4);
    $body = '';

    if (($h['transfer-encoding'] ?? '') === 'chunked') {
        $p = 0;

        while (true) {
            $eol = strpos($rest, "\r\n", $p);

            if ($eol === false) break;
            $size = (int) hexdec(trim(substr($rest, $p, $eol - $p)));

            if ($size === 0) break;
            $p = $eol + 2;

            if (strlen($rest) - $p < $size + 2) break;   /* chunk still in flight */
            $body .= substr($rest, $p, $size);
            $p += $size + 2;
        }
    } else {
        $body = $rest;
    }

    return [$h, $body];
}

/* One request on its own connection, read to EOF. */
function tas_h1_fire(int $port, string $req): string
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 5);
    fwrite($fp, $req);
    $raw = '';

    while (!feof($fp)) {
        $c = fread($fp, 8192);

        if ($c === '' || $c === false) break;
        $raw .= $c;
    }

    fclose($fp);
    return $raw;
}

/* GET /stream with the given Accept-Encoding, hold for $gate_ms while the
 * handler is parked, then release it and read to the end. Returns the
 * response headers, the body prefix that arrived before the release, and
 * the whole body — all three still encoded. */
function tas_flush_probe(int $port, string $encoding, int $gate_ms = 300): array
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 5);
    fwrite($fp,
        "GET /stream HTTP/1.1\r\n"
      . "Host: x\r\n"
      . "Accept-Encoding: $encoding\r\n"
      . "Connection: close\r\n\r\n");

    $st = ['raw' => '', 'mark' => 0];

    $reader = Async\spawn(function () use ($fp, &$st) {
        while (!feof($fp)) {
            $c = fread($fp, 8192);

            if ($c === '' || $c === false) break;
            $st['raw'] .= $c;
        }

        fclose($fp);
    });

    Async\delay($gate_ms);
    $st['mark'] = strlen($st['raw']);
    tas_h1_fire($port, "GET /release HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    Async\await($reader);

    [$h, $whole] = tas_h1_split($st['raw']);
    [, $early]   = tas_h1_split(substr($st['raw'], 0, $st['mark']));

    return [$h, $early, $whole];
}
