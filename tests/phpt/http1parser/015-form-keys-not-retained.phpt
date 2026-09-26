--TEST--
HTTP/1 parser: distinct query and form keys are released with their request
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* PHP's own registration interns every key until PHP request shutdown, which
 * this server never reaches: 300 requests of 100 fresh keys each held about
 * 4 MiB that way. Released with the request, they leave the body pool's one
 * cached slot (1 MiB) and noise. */

function retained(callable $request): int
{
    $request();   // warm the body pool and the parser
    gc_collect_cycles();
    $start = memory_get_usage();

    for ($r = 0; $r < 300; $r++) {
        $request();
    }

    gc_collect_cycles();
    return memory_get_usage() - $start;
}

$fresh_keys = static function (): string {
    $pairs = [];

    for ($i = 0; $i < 100; $i++) {
        $pairs[] = 'k' . bin2hex(random_bytes(16)) . '=1';
    }

    return implode('&', $pairs);
};

$query = retained(static function () use ($fresh_keys): void {
    TrueAsync\http_parse_request('GET /?' . $fresh_keys() . " HTTP/1.1\r\nHost: t\r\n\r\n")->getQuery();
});

$post = retained(static function () use ($fresh_keys): void {
    $body = $fresh_keys();
    TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\n"
        . "Content-Type: application/x-www-form-urlencoded\r\n"
        . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body)->getPost();
});

echo 'query under 256 KiB: ', $query < 256 * 1024 ? 'yes' : "no ($query)", "\n";
echo 'post under 256 KiB: ', $post < 256 * 1024 ? 'yes' : "no ($post)", "\n";
?>
--EXPECT--
query under 256 KiB: yes
post under 256 KiB: yes
