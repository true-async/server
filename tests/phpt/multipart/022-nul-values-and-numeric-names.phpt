--TEST--
Multipart: a field value keeps its NUL bytes, and a numeric field name reaches getFile()
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* The value copy used to stop at the first NUL while the length stayed whole,
 * so getPost() read that many bytes past the copy. */

$boundary = 'bnd';
$value    = "x\0" . str_repeat('y', 4000);
$body     = "--$boundary\r\nContent-Disposition: form-data; name=\"blob\"\r\n\r\n$value\r\n"
          . "--$boundary\r\nContent-Disposition: form-data; name=\"123\"; filename=\"n.txt\"\r\n"
          . "Content-Type: text/plain\r\n\r\nnumeric\r\n"
          . "--$boundary--\r\n";

$request = TrueAsync\http_parse_request(
    "POST / HTTP/1.1\r\nHost: t\r\n"
    . "Content-Type: multipart/form-data; boundary=$boundary\r\n"
    . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body
);

$post = $request->getPost();
echo 'blob length: ', strlen($post['blob']), "\n";
echo 'blob intact: ', $post['blob'] === $value ? 'yes' : 'no', "\n";
echo 'files keys: ', json_encode(array_keys($request->getFiles())), "\n";
echo 'getFile(123): ', $request->getFile('123')?->getClientFilename(), "\n";
?>
--EXPECT--
blob length: 4002
blob intact: yes
files keys: [123]
getFile(123): n.txt
