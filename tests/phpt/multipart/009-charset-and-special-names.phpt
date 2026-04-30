--TEST--
Multipart: Content-Type charset extraction + filename with spaces/quotes/unicode
--EXTENSIONS--
true_async_server
--FILE--
<?php
$bnd = '---bnd';

$body = "--$bnd\r\n"
      . "Content-Disposition: form-data; name=\"a\"; filename=\"hello world.txt\"\r\n"
      . "Content-Type: text/plain; charset=utf-8\r\n"
      . "\r\n"
      . "hi\r\n"
      . "--$bnd\r\n"
      . "Content-Disposition: form-data; name=\"b\"; filename=\"unicode-✓.txt\"\r\n"
      . "Content-Type: text/plain; charset=ISO-8859-1\r\n"
      . "\r\n"
      . "ok\r\n"
      . "--$bnd\r\n"
      . "Content-Disposition: form-data; name=\"c\"; filename=\"q'uoted.txt\"\r\n"
      . "Content-Type: text/plain\r\n"
      . "\r\n"
      . "z\r\n"
      . "--$bnd--\r\n";

$req_str = "POST / HTTP/1.1\r\nHost: t\r\n"
         . "Content-Type: multipart/form-data; boundary=$bnd\r\n"
         . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;

$req = TrueAsync\http_parse_request($req_str);

foreach (['a', 'b', 'c'] as $k) {
    $f = $req->getFile($k);
    echo "$k: name=" . $f->getClientFilename()
       . " type=" . var_export($f->getClientMediaType(), true)
       . " charset=" . var_export($f->getClientCharset(), true)
       . " size=" . $f->getSize()
       . "\n";
}
--EXPECT--
a: name=hello world.txt type='text/plain' charset='utf-8' size=2
b: name=unicode-✓.txt type='text/plain' charset='ISO-8859-1' size=2
c: name=q'uoted.txt type='text/plain' charset=NULL size=1
