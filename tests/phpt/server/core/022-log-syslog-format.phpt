--TEST--
HttpServer syslog formatter (#5, B5): bare RFC 5424 message (framing is the transport's)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_log_format_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* Stage B5: the syslog formatter renders the fixed canonical record as a bare
 * RFC 5424 message ("<PRI>1 TS HOST APP PROCID - - MSG"). PRI = facility
 * (user=1)*8 + severity (INFO=6) = 14. Framing is applied by the transport:
 * RFC 6587 octet count on TCP (covered by 023), one datagram on UDP/unix
 * (covered by 025) — so the formatter output carries no length prefix. */

$msg = _http_log_format_selftest('syslog');

$expMsg = '<14>1 2024-01-01T00:00:00.123Z ' . gethostname() . ' php-http-server '
        . getmypid() . ' - - '
        . 'user login path=/a b tag=v"1 line=a' . "\n"
        . 'b n=-7 sz=4294967296 ok=true r=1.5';

var_dump($msg === $expMsg);

echo "done\n";
?>
--EXPECT--
bool(true)
done
