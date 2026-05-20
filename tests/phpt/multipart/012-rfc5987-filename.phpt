--TEST--
Multipart: RFC 5987 filename* — percent-decoded UTF-8 filename + precedence over filename=
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* "файл.txt" UTF-8 bytes percent-encoded per RFC 5987 ext-value.
 * The legacy `filename=` carries a degraded ASCII form; per RFC 5987
 * §4.2 (and our parser), the `filename*` value takes precedence. */
$utf8     = "файл.txt";
$encoded  = "%D1%84%D0%B0%D0%B9%D0%BB.txt";

$cases = [
    /* [content-disposition value, expected client filename] */
    "filename* alone, UTF-8''"          =>
        ["form-data; name=\"a\"; filename*=UTF-8''$encoded",            $utf8],
    "filename* + filename, * wins"      =>
        ["form-data; name=\"b\"; filename=\"fallback.txt\"; filename*=UTF-8''$encoded", $utf8],
    /* RFC 5987 lang segment between the two single quotes — must be skipped. */
    "filename* with language tag"       =>
        ["form-data; name=\"c\"; filename*=UTF-8'ru'$encoded",          $utf8],
    /* Invalid ext-value (missing both single quotes): decoder returns
     * NULL, processor falls back to the plain `filename=` token. */
    "filename* malformed → fallback"    =>
        ["form-data; name=\"d\"; filename=\"plain.txt\"; filename*=garbage", "plain.txt"],
    /* Invalid %XX inside ext-value — decoder passes the bytes through
     * verbatim (decoder docstring guarantees this). */
    "filename* with bad %XX passthrough" =>
        ["form-data; name=\"e\"; filename*=UTF-8''bad%ZZok",            "bad%ZZok"],
];

$body = '';
foreach ($cases as $cd) {
    [$disp, $_] = $cd;
    $body .= "-----b\r\n" .
             "Content-Disposition: $disp\r\n" .
             "Content-Type: application/octet-stream\r\n" .
             "\r\n" .
             "x\r\n";
}
$body .= "-----b--\r\n";

$req_str = "POST /u HTTP/1.1\r\n" .
           "Host: t\r\n" .
           "Content-Type: multipart/form-data; boundary=---b\r\n" .
           "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;

$req   = TrueAsync\http_parse_request($req_str);
$files = $req->getFiles();

foreach ($cases as $label => [$_, $expected]) {
    $name = substr($label, 0, 1);  // first letter — matches name="a".."e" above
    /* Map case index → field name by reusing a→e ordering. */
}

$labels = ['a','b','c','d','e'];
$i = 0;
foreach ($cases as $label => [$_, $expected]) {
    $f = $files[$labels[$i]] ?? null;
    $got = $f ? $f->getClientFilename() : '<null>';
    echo $label, " → ", $got, " | ", ($got === $expected ? "OK" : "MISMATCH expected=$expected"), "\n";
    $i++;
}
?>
--EXPECT--
filename* alone, UTF-8'' → файл.txt | OK
filename* + filename, * wins → файл.txt | OK
filename* with language tag → файл.txt | OK
filename* malformed → fallback → plain.txt | OK
filename* with bad %XX passthrough → bad%ZZok | OK
