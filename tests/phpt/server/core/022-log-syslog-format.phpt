--TEST--
HttpServer syslog formatter (#5, B5): RFC 5424 message + RFC 6587 octet framing
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
/* Stage B5: the syslog formatter renders the fixed canonical record as an
 * RFC 5424 message ("<PRI>1 TS HOST APP PROCID - - MSG") wrapped in RFC 6587
 * octet-counted framing ("LEN SP MSG"). PRI = facility(user=1)*8 + severity
 * (INFO=6) = 14. The frame length must equal the message byte count so a TCP
 * receiver can split records even when MSG carries an embedded newline. */

$out = _http_log_format_selftest('syslog');

/* Split the octet frame: "LEN SP MSG". */
$sp  = strpos($out, ' ');
$len = (int) substr($out, 0, $sp);
$msg = substr($out, $sp + 1);

var_dump($len === strlen($msg));         // frame length matches the message

/* MSG is deterministic given this host/pid. */
$expMsg = '<14>1 2024-01-01T00:00:00.123Z ' . gethostname() . ' php-http-server '
        . getmypid() . ' - - '
        . 'user login path=/a b tag=v"1 line=a' . "\n"
        . 'b n=-7 sz=4294967296 ok=true r=1.5';

var_dump($msg === $expMsg);
var_dump($len === strlen($expMsg));

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
done
