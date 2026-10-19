--TEST--
Multipart: a client filename with path components is saved under its basename, as PHP saves it
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* PHP keeps the part of the client filename after the last '/' or '\'
 * (php_ap_basename, main/rfc1867.c) and saves the file; RFC 7578 §4.2 asks the
 * receiver to drop path information. The expected lines are what php-cgi
 * (8.6.0-dev) put in $_FILES['f']['name'], 'error' and the saved file for the
 * same Content-Disposition, checked on 2026-10-19. */

function upload(string $filename): TrueAsync\UploadedFile
{
    $b = '---bnd';
    $part = "--$b\r\nContent-Disposition: form-data; name=\"f\"; filename=\"$filename\"\r\n"
          . "Content-Type: text/plain\r\n\r\npayload\r\n--$b--\r\n";

    return TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\n"
        . "Content-Type: multipart/form-data; boundary=$b\r\n"
        . "Content-Length: " . strlen($part) . "\r\n\r\n" . $part)->getFile('f');
}

foreach ([
    '../etc/passwd',
    '/abs/path.txt',
    'C:\\Users\\me\\report.pdf',
    'a/b\\c.txt',
    'a\\b/c.txt',
    'my..report.pdf',
    'C:file.txt',
    'dir/',
] as $name) {
    $f = upload($name);
    $stream = $f->getStream();
    echo json_encode($name), " -> ", json_encode($f->getClientFilename()),
         " err=", $f->getError(),
         " body=", $stream === null ? 'none' : stream_get_contents($stream), "\n";
}
?>
--EXPECT--
"..\/etc\/passwd" -> "passwd" err=0 body=payload
"\/abs\/path.txt" -> "path.txt" err=0 body=payload
"C:\\Users\\me\\report.pdf" -> "report.pdf" err=0 body=payload
"a\/b\\c.txt" -> "c.txt" err=0 body=payload
"a\\b\/c.txt" -> "c.txt" err=0 body=payload
"my..report.pdf" -> "my..report.pdf" err=0 body=payload
"C:file.txt" -> "C:file.txt" err=0 body=payload
"dir\/" -> "" err=0 body=payload
