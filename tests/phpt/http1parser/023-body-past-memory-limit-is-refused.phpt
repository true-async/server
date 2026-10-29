--TEST--
HTTP/1 parser: a body past memory_limit is refused, and the script goes on without a pending memory-limit error
--EXTENSIONS--
true_async_server
--INI--
memory_limit=128M
--FILE--
<?php
/* Each body fits the 10 MiB body limit of http_parse_request(), but before
 * parsing memory_limit is lowered to leave 3 MiB of room. The buffer is
 * allocated with fallible allocations, so the request is refused (false)
 * instead of a delayed memory-limit error killing the script at its next
 * statement, or the hard limit (twice memory_limit) ending the process with
 * exit code 137. */

function parse_with_headroom(string $raw): mixed
{
    ini_set('memory_limit', (string)(memory_get_usage(true) + (3 << 20)));
    $request = \TrueAsync\http_parse_request($raw);
    ini_set('memory_limit', '128M');
    return $request;
}

/* The refusal is memory's: under the full limit the same request parses. */
function refused_for_memory(string $raw): string
{
    $refused = parse_with_headroom($raw) === false;
    $parsed  = \TrueAsync\http_parse_request($raw) instanceof \TrueAsync\HttpRequest;
    return $refused && $parsed ? 'refused for memory' : "refused=$refused parsed=$parsed";
}

$mib = str_repeat('x', 1 << 20);

/* Chunked: past 2 MiB the buffer reserves 4 MiB, then doubles */
$raw = "POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n"
     . str_repeat(dechex(strlen($mib)) . "\r\n" . $mib . "\r\n", 6) . "0\r\n\r\n";
echo refused_for_memory($raw), "\n";

/* Content-Length: one pre-allocation of the whole declared body */
$raw = "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: " . (9 << 20) . "\r\n\r\n"
     . str_repeat($mib, 9);
echo refused_for_memory($raw), "\n";

/* Multipart: a field value doubles past the limit while it is fed */
$body = "--b\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\n"
      . str_repeat($mib, 6) . "\r\n--b--\r\n";
$raw  = "POST / HTTP/1.1\r\nHost: t\r\nContent-Type: multipart/form-data; boundary=b\r\n"
      . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
unset($body);
echo refused_for_memory($raw), "\n";
unset($raw);

/* The same small request parses, and PHP code runs on: no fatal is pending */
$ok = parse_with_headroom("POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\nok");
var_dump($ok->getBody());

\TrueAsync\server_dispose();
echo "done\n";
?>
--EXPECT--
refused for memory
refused for memory
refused for memory
string(2) "ok"
done
