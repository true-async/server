<?php
/* Raw HTTP/1.1 helpers for the rate limit tests (core/089 onward).
 *
 * The tests watch the wire: a refusal must keep the connection, and a request
 * pipelined behind it must still be answered, which a client library would
 * hide. */

require_once __DIR__ . '/../_read_exact.inc';

/* One response off $fp: ['status' => int, 'headers' => [lower-case name =>
 * value], 'body' => string], or null when the peer closed first. Reads a body
 * framed by Content-Length or chunked; HEAD responses pass $head. */
function rl_read_response($fp, bool $head = false): ?array
{
    $head_block = '';

    while (!str_contains($head_block, "\r\n\r\n")) {
        $line = fgets($fp);

        if ($line === false) {
            return null;
        }

        $head_block .= $line;
    }

    $lines  = explode("\r\n", rtrim($head_block, "\r\n"));
    $status = (int) explode(' ', array_shift($lines), 3)[1];
    $headers = [];

    foreach ($lines as $line) {
        [$name, $value] = explode(':', $line, 2);
        $headers[strtolower($name)] = trim($value);
    }

    $body = '';

    if (!$head && isset($headers['content-length'])) {
        $body = tas_read_exact($fp, (int) $headers['content-length']);
    } elseif (!$head && ($headers['transfer-encoding'] ?? '') === 'chunked') {
        while (($size_line = fgets($fp)) !== false) {
            $size = hexdec(trim($size_line));

            if ($size === 0) {
                fgets($fp);
                break;
            }

            $body .= tas_read_exact($fp, $size);
            fgets($fp);
        }
    }

    return ['status' => $status, 'headers' => $headers, 'body' => $body];
}

/* The full waits the rules of these tests give, for rl_wait(), ascending. */
function rl_full_waits(): array
{
    return [15, 30, 60, 3600];
}

/* A Retry-After value as the test expects it. Under valgrind (run-tests -m
 * sets ZEND_DONT_UNLOAD_MODULES) a request takes long enough for the wait to
 * drop a few seconds below the full one, so a value up to 15 s (at most half
 * the wait) under a full wait reads as that wait there; any other run
 * compares it exactly. */
function rl_wait(int $seconds): int
{
    if (getenv('ZEND_DONT_UNLOAD_MODULES') === false) {
        return $seconds;
    }

    foreach (rl_full_waits() as $full) {
        if ($seconds <= $full && $seconds >= $full - min(15, intdiv($full, 2))) {
            return $full;
        }
    }

    return $seconds;
}

/* "429 retry-after=30 body=Too Many Requests" or "200 body=..." for one line
 * of expected output. */
function rl_describe(?array $response): string
{
    if ($response === null) {
        return 'closed';
    }

    $out = (string) $response['status'];

    if (isset($response['headers']['retry-after'])) {
        $out .= ' retry-after=' . rl_wait((int) $response['headers']['retry-after']);
    }

    return $out . ' body=' . trim($response['body']);
}

/* A keep-alive connection to 127.0.0.1:$port, with a read timeout. */
function rl_connect(int $port, string $host = '127.0.0.1')
{
    $target = str_contains($host, ':') ? "tcp://[$host]:$port" : "tcp://$host:$port";
    $fp = stream_socket_client($target, $errno, $errstr, 2);

    if ($fp === false) {
        throw new RuntimeException("connect $target: $errstr");
    }

    stream_set_timeout($fp, 5);
    return $fp;
}

/* Sends one GET on $fp and reads its response. */
function rl_get($fp, string $path = '/', array $headers = []): ?array
{
    $extra = '';

    foreach ($headers as $name => $value) {
        $extra .= "$name: $value\r\n";
    }

    fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\n$extra\r\n");
    return rl_read_response($fp);
}
