--TEST--
HttpServer log json (#5 / #130): invalid UTF-8 in a field becomes U+FFFD, valid UTF-8 passes through
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
/* A JSON string must be valid UTF-8 (RFC 8259), but the Body/Attributes carry
 * raw client bytes — the request path, header values. A well-formed multi-byte
 * sequence has to survive intact; an ill-formed byte is replaced with U+FFFD,
 * the same substitution php_json_encode makes under JSON_INVALID_UTF8_SUBSTITUTE.
 * Without it a single 0xFF in a request target would make the whole log line
 * invalid UTF-8 and a strict ingester (Elasticsearch, Loki) could reject the
 * record or its batch (#130). */

$FFFD = "\u{FFFD}";

/* é (2-byte), € (3-byte), 😀 (4-byte) are all valid and must pass through.
 * 0xFF is not a legal lead byte; 0x80 is a bare continuation — each is one
 * ill-formed byte and becomes one U+FFFD. */
$body = "\u{e9}\u{20ac}\u{1f600}\xff\x80end";

$json = _http_log_format_selftest('json', false, '', $body);

/* The bad bytes did not split the record: still one line. */
var_dump(substr_count($json, "\n") === 1);

/* The line is valid JSON over valid UTF-8, so json_decode accepts it. */
$j = json_decode(rtrim($json, "\n"), true);
var_dump(is_array($j));
var_dump(json_last_error() === JSON_ERROR_NONE);

/* Valid code points intact; each ill-formed byte replaced by exactly one U+FFFD. */
var_dump($j['Body'] === "\u{e9}\u{20ac}\u{1f600}{$FFFD}{$FFFD}end");

/* 0xFF appears in neither a valid sequence nor U+FFFD (EF BF BD), so a raw one
 * surviving into the output would mean the substitution was skipped. */
var_dump(strpos($json, "\xff") === false);

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
done
