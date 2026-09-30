--TEST--
Multipart: a MAX_FILE_SIZE field refuses the larger files after it with UPLOAD_ERR_FORM_SIZE
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* PHP reads a MAX_FILE_SIZE field and holds the files after it to that size;
 * a larger one reports UPLOAD_ERR_FORM_SIZE and keeps no temp file. */

$b    = 'bnd';
$part = fn(string $name, ?string $file, string $data) => "--$b\r\n"
    . "Content-Disposition: form-data; name=\"$name\"" . ($file !== null ? "; filename=\"$file\"" : '')
    . "\r\n\r\n$data\r\n";
$body = $part('before', 'a.bin', str_repeat('a', 100))
      . $part('MAX_FILE_SIZE', null, '50')
      . $part('big', 'b.bin', str_repeat('b', 100))
      . $part('small', 'c.bin', str_repeat('c', 50))
      . "--$b--\r\n";

$req = TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\n"
    . "Content-Type: multipart/form-data; boundary=$b\r\n"
    . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body);

foreach (['before', 'big', 'small'] as $name) {
    $f = $req->getFile($name);
    echo "$name: error=", $f->getError(),
        $f->getError() === UPLOAD_ERR_FORM_SIZE ? ' (UPLOAD_ERR_FORM_SIZE)' : '',
        " size=", var_export($f->getSize(), true), "\n";
}

echo "MAX_FILE_SIZE field: ", $req->getPost()['MAX_FILE_SIZE'] ?? 'missing', "\n";
?>
--EXPECT--
before: error=0 size=100
big: error=2 (UPLOAD_ERR_FORM_SIZE) size=NULL
small: error=0 size=50
MAX_FILE_SIZE field: 50
