--TEST--
Multipart: malformed inputs + multiple files with same field name
--EXTENSIONS--
true_async_server
--FILE--
<?php
// Helper: parse a request and return the count + any throwable
function tryParse(string $req): string {
    try {
        $r = TrueAsync\http_parse_request($req);
        $f = $r->getFiles();
        return 'ok files=' . count($f);
    } catch (\Throwable $e) {
        return 'throw ' . get_class($e);
    }
}

// CASE 1: missing trailing boundary (-- after last) — should still parse
// what it could, or throw cleanly.
$bnd = '---X';
$body1 = "--$bnd\r\n"
       . "Content-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\n"
       . "Content-Type: text/plain\r\n\r\nhello\r\n"
       . "--$bnd\r\n";  // no terminating "--"
$req1 = "POST / HTTP/1.1\r\nHost: t\r\n"
      . "Content-Type: multipart/form-data; boundary=$bnd\r\n"
      . "Content-Length: " . strlen($body1) . "\r\n\r\n" . $body1;
echo "missing_trailer: " . tryParse($req1) . "\n";

// CASE 2: multipart with no boundary parameter
$body2 = "--bnd\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nv\r\n--bnd--\r\n";
$req2 = "POST / HTTP/1.1\r\nHost: t\r\n"
      . "Content-Type: multipart/form-data\r\n"  // no boundary=
      . "Content-Length: " . strlen($body2) . "\r\n\r\n" . $body2;
echo "no_boundary: " . tryParse($req2) . "\n";

// CASE 3: duplicate field names — array semantics
$body3 = "--$bnd\r\n"
       . "Content-Disposition: form-data; name=\"items[]\"\r\n\r\nA\r\n"
       . "--$bnd\r\n"
       . "Content-Disposition: form-data; name=\"items[]\"\r\n\r\nB\r\n"
       . "--$bnd\r\n"
       . "Content-Disposition: form-data; name=\"items[]\"\r\n\r\nC\r\n"
       . "--$bnd--\r\n";
$req3 = "POST / HTTP/1.1\r\nHost: t\r\n"
      . "Content-Type: multipart/form-data; boundary=$bnd\r\n"
      . "Content-Length: " . strlen($body3) . "\r\n\r\n" . $body3;
$r = TrueAsync\http_parse_request($req3);
$post = $r->getPost();
echo "post_is_array=" . (is_array($post) ? 1 : 0) . "\n";

// CASE 4: file with name="x" repeated — last one wins (or array depending on impl)
$body4 = "--$bnd\r\n"
       . "Content-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\n"
       . "Content-Type: text/plain\r\n\r\nfirst\r\n"
       . "--$bnd\r\n"
       . "Content-Disposition: form-data; name=\"f\"; filename=\"b.txt\"\r\n"
       . "Content-Type: text/plain\r\n\r\nsecond\r\n"
       . "--$bnd--\r\n";
$req4 = "POST / HTTP/1.1\r\nHost: t\r\n"
      . "Content-Type: multipart/form-data; boundary=$bnd\r\n"
      . "Content-Length: " . strlen($body4) . "\r\n\r\n" . $body4;
$r4 = TrueAsync\http_parse_request($req4);
$files = $r4->getFiles();
echo "dup_keys=" . implode(',', array_keys($files)) . "\n";
$f = $files['f'];
echo "dup_kind=" . (is_array($f) ? 'array' : get_class($f)) . "\n";
--EXPECTF--
missing_trailer: %s
no_boundary: %s
post_is_array=1
dup_keys=%s
dup_kind=%s
