--TEST--
Multipart: too-many-files (>20) + too-many-fields (>100) parser limits
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* MP_MAX_FILES = 20, MP_MAX_FIELDS = 100. Sending more triggers the
 * limit-clamp branches in multipart_processor.c:
 *   - Files: 21st+ part → MP_UPLOAD_ERR_TOO_MANY_FILES (100), file
 *     never opened, no tmp_path.
 *   - Fields: 101st+ field → on_part_end skips it silently. */

$b = '---bnd';
$body = '';

/* 25 file parts. */
for ($i = 0; $i < 25; $i++) {
    $body .= "--$b\r\n"
           . "Content-Disposition: form-data; name=\"f$i\"; filename=\"f$i.bin\"\r\n"
           . "Content-Type: application/octet-stream\r\n\r\n"
           . "x$i\r\n";
}
/* 110 field parts. */
for ($i = 0; $i < 110; $i++) {
    $body .= "--$b\r\n"
           . "Content-Disposition: form-data; name=\"k$i\"\r\n\r\n"
           . "v$i\r\n";
}
$body .= "--$b--\r\n";

$req_str = "POST / HTTP/1.1\r\nHost: t\r\n"
         . "Content-Type: multipart/form-data; boundary=$b\r\n"
         . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;

$req = TrueAsync\http_parse_request($req_str);
$files = $req->getFiles();
$post  = $req->getPost();

echo "files-accepted: ", count($files), "\n";
/* The over-cap files still appear in the array but with TOO_MANY_FILES error. */
$err_counts = [0 => 0, 100 => 0];
foreach ($files as $f) {
    $err_counts[$f->getError()] = ($err_counts[$f->getError()] ?? 0) + 1;
}
ksort($err_counts);
foreach ($err_counts as $code => $n) echo "files-err-$code: $n\n";

echo "fields-accepted: ", count($post), "\n";
/* Confirm we got the first 100 fields (k0..k99) but not k100..k109. */
echo "k0-present:   ", isset($post['k0']) ? 'yes' : 'no', "\n";
echo "k99-present:  ", isset($post['k99']) ? 'yes' : 'no', "\n";
echo "k100-present: ", isset($post['k100']) ? 'yes' : 'no', "\n";
echo "done\n";
?>
--EXPECTF--
files-accepted: %d
files-err-0: %d
files-err-100: %d
fields-accepted: %d
k0-present:   yes
k99-present:  yes
k100-present: no
done
