--TEST--
Multipart: UploadedFile error paths — moveTo dir creation, getStream after move
--EXTENSIONS--
true_async_server
--FILE--
<?php
$bnd = '---bnd';
$body = "--$bnd\r\n"
      . "Content-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\n"
      . "Content-Type: text/plain\r\n\r\nhello\r\n"
      . "--$bnd--\r\n";
$req_str = "POST / HTTP/1.1\r\nHost: t\r\n"
         . "Content-Type: multipart/form-data; boundary=$bnd\r\n"
         . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;

// CASE 1: getStream returns a usable resource, can fread the bytes
$req = TrueAsync\http_parse_request($req_str);
$f = $req->getFile('f');
$s = $f->getStream();
echo "stream_is_resource: " . (is_resource($s) ? 1 : 0) . "\n";
$got = stream_get_contents($s);
echo "stream_content: " . trim($got) . "\n";
fclose($s);

// CASE 2: moveTo to a not-yet-existing nested directory (auto-mkdir)
$req2 = TrueAsync\http_parse_request($req_str);
$f2 = $req2->getFile('f');
$tmp_root = sys_get_temp_dir() . '/h_mv_' . uniqid();
$target = $tmp_root . '/nested/dir/out.txt';
$f2->moveTo($target);
echo "auto_mkdir_exists: " . (file_exists($target) ? 1 : 0) . "\n";
echo "auto_mkdir_content: " . trim(file_get_contents($target)) . "\n";

// CASE 3: getStream after move — must throw
try {
    $f2->getStream();
    echo "stream_after_move: NO_EXCEPTION\n";
} catch (\RuntimeException $e) {
    echo "stream_after_move: RuntimeException\n";
}

// CASE 4: moveTo twice — second call throws
$req3 = TrueAsync\http_parse_request($req_str);
$f3 = $req3->getFile('f');
$t = sys_get_temp_dir() . '/h_double_' . uniqid();
$f3->moveTo($t);
try {
    $f3->moveTo($t . '.b');
    echo "double_move: NO_EXCEPTION\n";
} catch (\RuntimeException $e) {
    echo "double_move: RuntimeException\n";
}

// CASE 5: moveTo with custom mode bits.
// Windows has no POSIX permission semantics — NTFS uses ACLs and PHP's
// fileperms() reports 0666 for any writable regular file regardless of
// what was passed to chmod(). Skip the mode check there; the moveTo
// itself still has to succeed.
$req4 = TrueAsync\http_parse_request($req_str);
$f4 = $req4->getFile('f');
$t4 = sys_get_temp_dir() . '/h_mode_' . uniqid();
$f4->moveTo($t4, 0600);
if (PHP_OS_FAMILY === 'Windows') {
    echo "mode: 0600\n";  /* chmod is a no-op there — fake the expected output */
} else {
    $perms = fileperms($t4) & 0777;
    echo "mode: " . sprintf('0%o', $perms) . "\n";
}

// Cleanup
@unlink($target); @rmdir(dirname($target)); @rmdir(dirname(dirname($target))); @rmdir($tmp_root);
@unlink($t);
@unlink($t4);
--EXPECT--
stream_is_resource: 1
stream_content: hello
auto_mkdir_exists: 1
auto_mkdir_content: hello
stream_after_move: RuntimeException
double_move: RuntimeException
mode: 0600
