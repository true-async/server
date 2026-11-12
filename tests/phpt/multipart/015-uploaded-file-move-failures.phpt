--TEST--
Multipart: UploadedFile::moveTo() — copy-fallback failure + mkdir failure
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Exercises uploaded_file.c moveTo failure tails:
 *   - L274-280: stat(dir) fails AND mkdir_recursive fails →
 *               "Cannot create directory"
 *   - L300-309: rename fails, copy_file fails too →
 *               "Failed to move file"
 *
 * A regular file stands in for a directory: nothing can be created below
 * it, on POSIX and on Windows alike. */

/* Holds onto the HttpRequest so the tmp file isn't reaped at scope exit. */
function parse_simple_upload(string $content = 'hello'): array {
    $b = '---bnd';
    $part = "--$b\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\n"
          . "Content-Type: text/plain\r\n\r\n$content\r\n--$b--\r\n";
    $req_str = "POST / HTTP/1.1\r\nHost: t\r\n"
         . "Content-Type: multipart/form-data; boundary=$b\r\n"
         . "Content-Length: " . strlen($part) . "\r\n\r\n" . $part;
    $req = TrueAsync\http_parse_request($req_str);
    return [$req, $req->getFile('f')];
}

function expect_runtime(string $label, callable $fn): void
{
    try {
        $fn();
        echo "$label: NO-THROW\n";
    } catch (\RuntimeException $e) {
        $msg = $e->getMessage();
        /* Print stable prefix only — actual errno-strings vary by libc. */
        $head = preg_replace('/:.*/', '', $msg);
        echo "$label: $head\n";
    }
}

$plain = sys_get_temp_dir() . '/upl-plain-' . getmypid() . '-' . bin2hex(random_bytes(4));
file_put_contents($plain, 'not a directory');

/* CASE 1: the parent does not exist and mkdir fails, since the path
 * runs through a regular file. */
[$req1, $f1] = parse_simple_upload();
expect_runtime('mkdir-fail',
    fn() => $f1->moveTo($plain . '/sub/x.txt'));

/* CASE 2: the parent exists but is a regular file, so stat succeeds,
 * rename fails and the copy fallback's fopen fails too: "Failed to move
 * file". */
[$req2, $f2] = parse_simple_upload();
expect_runtime('move-fail',
    fn() => $f2->moveTo($plain . '/inside-file.txt'));
@unlink($plain);

/* CASE 3: control — successful move (sanity check that the above
 * triggered the failure tails, not some earlier validation). */
[$req3, $f3] = parse_simple_upload('control-body');
$dest = sys_get_temp_dir() . '/upl-ctrl-' . getmypid() . '-' . bin2hex(random_bytes(4));
$f3->moveTo($dest);
echo "control: ", file_get_contents($dest), "\n";
@unlink($dest);

echo "done\n";
?>
--EXPECTF--
mkdir-fail: Cannot create directory
move-fail: Failed to move file
control: control-body
done
