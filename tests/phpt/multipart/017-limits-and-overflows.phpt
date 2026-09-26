--TEST--
Multipart: a form past max_input_vars is refused; files past the file limit answer per file
--EXTENSIONS--
true_async_server
--INI--
max_input_vars=100
--FILE--
<?php
/* Fields count against max_input_vars, and one more than it refuses the whole
 * request: a form missing a field reads as another form (#318). The parser
 * used to keep the first 100 fields and drop the rest without a word.
 *
 * Files keep PHP's answer instead: past MP_MAX_FILES (20) each extra file
 * carries MP_UPLOAD_ERR_TOO_MANY_FILES (100) and no temp file. */

function multipart_request(int $files, int $fields): string
{
    $b    = '---bnd';
    $body = '';

    for ($i = 0; $i < $files; $i++) {
        $body .= "--$b\r\n"
               . "Content-Disposition: form-data; name=\"f$i\"; filename=\"f$i.bin\"\r\n"
               . "Content-Type: application/octet-stream\r\n\r\n"
               . "x$i\r\n";
    }

    for ($i = 0; $i < $fields; $i++) {
        $body .= "--$b\r\n"
               . "Content-Disposition: form-data; name=\"k$i\"\r\n\r\n"
               . "v$i\r\n";
    }

    $body .= "--$b--\r\n";

    return "POST / HTTP/1.1\r\nHost: t\r\n"
         . "Content-Type: multipart/form-data; boundary=$b\r\n"
         . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
}

$req = TrueAsync\http_parse_request(multipart_request(25, 100));
$files = $req->getFiles();
$err_counts = [];

foreach ($files as $f) {
    $err_counts[$f->getError()] = ($err_counts[$f->getError()] ?? 0) + 1;
}

ksort($err_counts);
echo "files: ", count($files), "\n";
foreach ($err_counts as $code => $n) echo "files-err-$code: $n\n";
echo "fields at the limit: ", count($req->getPost()), "\n";

$over = TrueAsync\http_parse_request(multipart_request(0, 101));
echo "one field past the limit: ", $over === false ? 'refused by the parser' : 'accepted', "\n";
echo "done\n";
?>
--EXPECT--
files: 25
files-err-0: 20
files-err-100: 5
fields at the limit: 100
one field past the limit: refused by the parser
done
