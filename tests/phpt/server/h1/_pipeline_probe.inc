<?php
/* Pipelined client for the file-body failure tests. Included, it defines
 * h1_pipeline_probe(), which runs this file as a child process (a TLS client
 * cannot share the server's thread) and returns the child's one line. Run
 * directly, it sends a request for argv[3] and, pipelined behind it in the
 * same write, a `Connection: close` request for argv[4], then reads until EOF
 * or a 4 s timeout and prints one line about the first response and whatever
 * followed it:
 *
 *   status=200 declared=2097152 got=4096 extra=none end=closed
 *
 * got is the count of body bytes before EOF or before the next status line;
 * extra is the status of a response found glued after them, or "none"; end is
 * "closed" on EOF and "open" when the read timed out. */

function h1_pipeline_probe(string $scheme, int $port, string $first, string $second): string
{
    $proc = proc_open([PHP_BINARY, '-n', __FILE__, $scheme, (string) $port, $first, $second],
        [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
    $out = stream_get_contents($pipes[1]) . stream_get_contents($pipes[2]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($proc);

    return $out;
}

if (realpath($_SERVER['argv'][0] ?? '') !== __FILE__) {
    return;
}

[, $scheme, $port, $first, $second] = $argv;

$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$c = stream_socket_client("$scheme://127.0.0.1:$port", $errno, $errstr, 4, STREAM_CLIENT_CONNECT, $ctx);

if ($c === false) {
    echo "connect failed: $errstr\n";
    exit(1);
}

fwrite($c, "GET $first HTTP/1.1\r\nHost: t\r\n\r\n"
    . "GET $second HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");

stream_set_timeout($c, 4);
$in = '';

while (!feof($c)) {
    $chunk = fread($c, 65536);

    if ($chunk === false || $chunk === '') {
        if (stream_get_meta_data($c)['timed_out']) {
            break;
        }

        continue;
    }

    $in .= $chunk;
}

$end = feof($c) ? 'closed' : 'open';
$head_end = strpos($in, "\r\n\r\n");

if ($head_end === false) {
    echo "status=none declared=? got=0 extra=none end=$end\n";
    exit(0);
}

$head = substr($in, 0, $head_end);
$rest = substr($in, $head_end + 4);
preg_match('~^HTTP/1\.1 (\d+)~', $head, $status);
preg_match('~\r\ncontent-length: *(\d+)~i', $head, $declared);

$next = strpos($rest, "HTTP/1.1 ");
$got = $next === false ? strlen($rest) : $next;
$extra = $next === false ? 'none' : substr($rest, $next + 9, 3);

printf("status=%s declared=%s got=%d extra=%s end=%s\n",
    $status[1] ?? '?', $declared[1] ?? '?', $got, $extra, $end);
