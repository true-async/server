--TEST--
HttpServer log formatters (#5, B2): plain / logfmt / json golden output + escaping
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
/* Stage B2: the three built-in formatters render a fixed canonical record
 * (INFO "user login", a mix of attrs incl. space/quote/newline, a trace
 * context) with no server. plain stays byte-identical to B1; logfmt quotes
 * values that need it; json emits one valid OTel-Logs object per line and is
 * the only formatter that carries the trace fields. */

$nl = "\n";

$expPlain = '2024-01-01T00:00:00.123Z INFO user login path=/a b tag=v"1 line=a' . $nl
          . 'b n=-7 sz=4294967296 ok=true r=1.5' . $nl;

$expLogfmt = 'ts=2024-01-01T00:00:00.123Z level=INFO msg="user login" path="/a b" tag="v\"1" line="a' . $nl
           . 'b" n=-7 sz=4294967296 ok=true r=1.5' . $nl;

var_dump(_http_log_format_selftest('plain')  === $expPlain);
var_dump(_http_log_format_selftest('logfmt') === $expLogfmt);

/* json: valid, single line (embedded newline escaped), fields round-trip. */
$json = _http_log_format_selftest('json');
var_dump(substr_count($json, "\n") === 1);        // only the trailing newline

$j = json_decode(rtrim($json, "\n"), true);
var_dump(is_array($j));
var_dump($j['Timestamp'] === '2024-01-01T00:00:00.123Z');
var_dump($j['SeverityNumber'] === 9);
var_dump($j['SeverityText'] === 'INFO');
var_dump($j['Body'] === 'user login');
var_dump($j['Attributes'] === [
    'path' => '/a b',
    'tag'  => 'v"1',
    'line' => "a\nb",
    'n'    => -7,
    'sz'   => 4294967296,
    'ok'   => true,
    'r'    => 1.5,
]);
var_dump($j['TraceId'] === '000102030405060708090a0b0c0d0e0f');
var_dump($j['SpanId'] === '0001020304050607');

var_dump(_http_log_format_selftest('nope'));       // unknown style → false

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)
done
