--TEST--
Multipart: with file_uploads off every file part is left out and the fields arrive
--EXTENSIONS--
true_async_server
--INI--
file_uploads=0
--FILE--
<?php
/* PHP's file_uploads=Off skips every file part of a multipart body; the form
 * fields around them are parsed as usual. */

$b    = 'bnd';
$body = "--$b\r\nContent-Disposition: form-data; name=\"doc\"; filename=\"a.txt\"\r\n\r\nhello\r\n"
      . "--$b\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nkept\r\n"
      . "--$b--\r\n";

$req = TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\n"
    . "Content-Type: multipart/form-data; boundary=$b\r\n"
    . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body);

echo "files: ", count($req->getFiles()), "\n";
echo "title: ", $req->getPost()['title'] ?? 'missing', "\n";
?>
--EXPECT--
files: 0
title: kept
