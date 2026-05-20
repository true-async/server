--TEST--
Multipart: UploadedFile error states — INVALID_NAME, NO_FILE + getters on errored files
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Exercises uploaded_file.c getter branches on a file in error state:
 *   - getSize()      → null when error != OK
 *   - getStream()    → returns null on error (only throws on already-moved)
 *   - moveTo()       → throws with "upload error %d" message
 *   - isValid()      → false
 *   - isReady()      → false
 *
 * Plus getClientCharset() (covered nowhere else in the suite). */

function build_request(string $disp, string $type = 'application/octet-stream', string $body = 'x'): string
{
    $b = '---bnd';
    $part = "--$b\r\nContent-Disposition: $disp\r\nContent-Type: $type\r\n\r\n$body\r\n--$b--\r\n";
    return "POST / HTTP/1.1\r\nHost: t\r\n"
         . "Content-Type: multipart/form-data; boundary=$b\r\n"
         . "Content-Length: " . strlen($part) . "\r\n\r\n" . $part;
}

/* MP_UPLOAD_ERR_INVALID_NAME (=101) via path-traversal in filename */
$req = TrueAsync\http_parse_request(build_request(
    'form-data; name="x"; filename="../etc/passwd"'));
$f = $req->getFile('x');
echo "traversal:err=", $f->getError(),
     " size=", var_export($f->getSize(), true),
     " valid=", $f->isValid() ? 1 : 0,
     " ready=", $f->isReady() ? 1 : 0, "\n";

$s = $f->getStream();
echo "getStream: ", $s === null ? 'null' : 'resource', "\n";

try { $f->moveTo('/tmp/x'); echo "moveTo: NO_THROW\n"; }
catch (\RuntimeException $e) { echo "moveTo: ", $e->getMessage(), "\n"; }

/* MP_UPLOAD_ERR_INVALID_NAME via absolute path */
$req = TrueAsync\http_parse_request(build_request(
    'form-data; name="y"; filename="/abs/path"'));
$f = $req->getFile('y');
echo "abs-path:err=", $f->getError(), "\n";

/* MP_UPLOAD_ERR_INVALID_NAME via overlong filename (> 4096) */
$long = str_repeat('a', 5000);
$req = TrueAsync\http_parse_request(build_request(
    "form-data; name=\"z\"; filename=\"$long\""));
$f = $req->getFile('z');
echo "long-name:err=", $f->getError(), "\n";

/* MP_UPLOAD_ERR_NO_FILE via filename="" (RFC 7578 §4.2 — empty filename
 * is the way browsers signal "no file selected" for <input type=file>) */
$req = TrueAsync\http_parse_request(build_request(
    'form-data; name="empty"; filename=""'));
$f = $req->getFile('empty');
echo "empty-name:err=", $f->getError(),
     " ready=", $f->isReady() ? 1 : 0, "\n";

/* getClientCharset — extracted from Content-Type "...; charset=utf-8" */
$req = TrueAsync\http_parse_request(build_request(
    'form-data; name="t"; filename="a.txt"',
    'text/plain; charset=utf-8',
    'hello'));
$f = $req->getFile('t');
echo "charset:err=", $f->getError(),
     " ct=", $f->getClientMediaType(),
     " cs=", var_export($f->getClientCharset(), true), "\n";

/* No-charset → getClientCharset returns null. */
$req = TrueAsync\http_parse_request(build_request(
    'form-data; name="u"; filename="a.txt"',
    'text/plain',
    'hi'));
$f = $req->getFile('u');
echo "no-charset:cs=", var_export($f->getClientCharset(), true), "\n";

echo "done\n";
?>
--EXPECTF--
traversal:err=101 size=NULL valid=0 ready=0
getStream: null
moveTo: Cannot move file: upload error 101
abs-path:err=101
long-name:err=101
empty-name:err=4 ready=0
charset:err=0 ct=text/plain cs='utf-8'
no-charset:cs=NULL
done
