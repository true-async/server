<?php
/* Shared helper for the gRPC-over-HTTP/2 tests of a refused request body (#426).
 * Needs _h2_skipif.inc for h2_curl_bin() and h2_dev_null(). */

/* POSTs $body to /svc/Upload with curl and returns curl's -v trace (stderr).
 * The first byte goes out with the request and the rest $delay_us later, so a
 * handler is parked on the body when the bulk arrives. curl reads the upload
 * from stdin (-T -) and sends no HEADERS before its first byte; a plain pipe
 * would hold the request back. No shell: the argument array and the pipes work
 * the same on Windows, which has no head, sleep or tail. */
function grpc_h2_upload_timed(int $port, string $body, int $delay_us = 300000): string
{
    $spec = [
        0 => ['pipe', 'r'],
        1 => ['pipe', 'w'],
        2 => ['pipe', 'w'],
    ];
    $proc = proc_open([
        h2_curl_bin(), '--http2-prior-knowledge', '-s', '-v', '--max-time', '5',
        '-X', 'POST', '-T', '-',
        '-H', 'content-type: application/grpc', '-H', 'te: trailers',
        '-o', h2_dev_null(), "http://127.0.0.1:$port/svc/Upload",
    ], $spec, $pipes);
    if (!is_resource($proc)) {
        return '';
    }

    fwrite($pipes[0], substr($body, 0, 1));
    fflush($pipes[0]);
    usleep($delay_us);
    fwrite($pipes[0], substr($body, 1));
    fclose($pipes[0]);

    $trace = (string) stream_get_contents($pipes[2]);
    fclose($pipes[2]);
    stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    proc_close($proc);

    return $trace;
}
