--TEST--
Multipart: a parsed form is held in PHP's request memory, which memory_limit bounds, once and not twice
--EXTENSIONS--
true_async_server
--INI--
memory_limit=256M
--FILE--
<?php
/* The parser keeps each field value until the request ends. That memory is the
 * request's: memory_get_usage() sees it and memory_limit bounds it. The value
 * buffer grows by doubling while the field arrives, and is cut to the value
 * when the field ends, so a value costs about its own size. */

/* Just past 4 MiB: the doubled buffer is 8 MiB, and the fitted value stays
 * under 1.5x even on Windows, which rounds a huge block up to 2 MiB. */
$size  = (4 << 20) + (64 << 10);
$body  = "--b\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\n"
       . str_repeat('x', $size)
       . "\r\n--b--\r\n";
$raw   = "POST / HTTP/1.1\r\nHost: t\r\n"
       . "Content-Type: multipart/form-data; boundary=b\r\n"
       . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
unset($body);

$base    = memory_get_usage();
$request = TrueAsync\http_parse_request($raw);
$held    = memory_get_usage() - $base;

echo "held at least the value: ", $held >= $size ? 'yes' : "no ($held)", "\n";
echo "held under 1.5x the value: ", $held < $size * 1.5 ? 'yes' : "no ($held)", "\n";
echo "value: ", strlen($request->getPost()['f']), "\n";
?>
--EXPECT--
held at least the value: yes
held under 1.5x the value: yes
value: 4259840
