--TEST--
Multipart: parser pool reuse across requests (Phase 6 struct split)
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Phase 6 moved multipart state (multipart_proc, post_data, files) from
 * http1_parser_t into http_request_t. This regression test parses two
 * multipart requests in a row so the parser returns to the pool and is
 * reused: if http_request_destroy or http_parser_reset_for_reuse fail to
 * release the multipart state, the second parse will either leak, crash,
 * or return stale data from the first parse.
 */

function build_multipart(string $fieldName, string $value): string {
    $body = "-----boundary\r\n" .
            "Content-Disposition: form-data; name=\"{$fieldName}\"\r\n" .
            "\r\n" .
            $value . "\r\n" .
            "-----boundary--\r\n";

    return "POST / HTTP/1.1\r\n" .
           "Host: test\r\n" .
           "Content-Type: multipart/form-data; boundary=---boundary\r\n" .
           "Content-Length: " . strlen($body) . "\r\n" .
           "\r\n" .
           $body;
}

$r1 = TrueAsync\http_parse_request(build_multipart('first', 'alpha'));
var_dump($r1->getPost());
unset($r1);

/* Second parse: parser has been returned to the pool and reset. The new
 * request must start with an empty post_data/files table. */
$r2 = TrueAsync\http_parse_request(build_multipart('second', 'beta'));
var_dump($r2->getPost());
var_dump($r2->getFiles());
?>
--EXPECT--
array(1) {
  ["first"]=>
  string(5) "alpha"
}
array(1) {
  ["second"]=>
  string(4) "beta"
}
array(0) {
}
