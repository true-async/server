--TEST--
Multipart: edge cases — empty file, binary content, no Content-Type
--EXTENSIONS--
true_async_server
--FILE--
<?php
$boundary = '---bnd';
$binary = pack('C*', 0, 1, 2, 3, 4, 255, 254, 253, 0, 13, 10, 0); // 12 bytes incl. NULs/CRLF

$body = "--$boundary\r\n"
      . "Content-Disposition: form-data; name=\"empty\"; filename=\"e.txt\"\r\n"
      . "Content-Type: text/plain\r\n"
      . "\r\n"
      . "\r\n"
      . "--$boundary\r\n"
      . "Content-Disposition: form-data; name=\"bin\"; filename=\"b.dat\"\r\n"
      . "Content-Type: application/octet-stream\r\n"
      . "\r\n"
      . $binary . "\r\n"
      . "--$boundary\r\n"
      . "Content-Disposition: form-data; name=\"notype\"; filename=\"n.bin\"\r\n"
      . "\r\n"
      . "abc\r\n"
      . "--$boundary--\r\n";

$req_str = "POST / HTTP/1.1\r\nHost: t\r\n"
         . "Content-Type: multipart/form-data; boundary=$boundary\r\n"
         . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;

$req = TrueAsync\http_parse_request($req_str);
$files = $req->getFiles();

foreach (['empty', 'bin', 'notype'] as $k) {
    $f = $files[$k];
    echo "$k: name=" . $f->getClientFilename()
       . " size=" . $f->getSize()
       . " type=" . var_export($f->getClientMediaType(), true)
       . " err=" . $f->getError()
       . " valid=" . ($f->isValid() ? 1 : 0)
       . "\n";
}

// Verify binary content round-trips byte-for-byte
$tmp = sys_get_temp_dir() . '/h_bin_' . uniqid();
$files['bin']->moveTo($tmp);
$got = file_get_contents($tmp);
echo "bin_match: " . ($got === $binary ? 'yes' : 'no') . "\n";
echo "bin_bytes: " . bin2hex($got) . "\n";
unlink($tmp);
--EXPECT--
empty: name=e.txt size=0 type='text/plain' err=0 valid=1
bin: name=b.dat size=12 type='application/octet-stream' err=0 valid=1
notype: name=n.bin size=3 type=NULL err=0 valid=1
bin_match: yes
bin_bytes: 0001020304fffefd000d0a00
