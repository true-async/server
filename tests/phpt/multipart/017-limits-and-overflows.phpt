--TEST--
Multipart: a form past max_input_vars or max_multipart_body_parts is refused; files past max_file_uploads are left out
--EXTENSIONS--
true_async_server
--INI--
max_input_vars=100
--FILE--
<?php
/* Fields count against max_input_vars, and one more than it refuses the whole
 * request: a form missing a field reads as another form (#318). Parts count
 * against max_multipart_body_parts, which is max_input_vars + max_file_uploads
 * (120 here) when unset, and one more refuses the request too.
 *
 * Files keep PHP's answer instead: past max_file_uploads (20) the extra files
 * are left out of the form, with no temp file (#322). */

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

$req = TrueAsync\http_parse_request(multipart_request(25, 0));
$files = $req->getFiles();
$err_counts = [];

foreach ($files as $f) {
    $err_counts[$f->getError()] = ($err_counts[$f->getError()] ?? 0) + 1;
}

ksort($err_counts);
echo "files: ", count($files), "\n";
foreach ($err_counts as $code => $n) echo "files-err-$code: $n\n";
echo "left out: ", isset($files['f20']) ? 'no' : 'yes', "\n";

$fields = TrueAsync\http_parse_request(multipart_request(0, 100));
echo "fields at the limit: ", count($fields->getPost()), "\n";

$over = TrueAsync\http_parse_request(multipart_request(0, 101));
echo "one field past the limit: ", $over === false ? 'refused by the parser' : 'accepted', "\n";

$parts = TrueAsync\http_parse_request(multipart_request(20, 100));
echo "parts at the limit: ", $parts === false ? 'refused' : count($parts->getFiles()) + count($parts->getPost()), "\n";

$over = TrueAsync\http_parse_request(multipart_request(21, 100));
echo "one part past the limit: ", $over === false ? 'refused by the parser' : 'accepted', "\n";
echo "done\n";
?>
--EXPECT--
files: 20
files-err-0: 20
left out: yes
fields at the limit: 100
one field past the limit: refused by the parser
parts at the limit: 120
one part past the limit: refused by the parser
done
