--TEST--
Multipart: parser error-reason paths via malformed boundary/headers/body
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Hits multipart_parser_execute MP_STATE_ERROR branches:
 *   - "expected CR or LF after boundary"   (after boundary, char ≠ CR/LF/'-')
 *   - "expected LF after CR"               (CR not followed by LF)
 *   - "unexpected end of line in header name"
 *   - "expected LF after CR in header"
 *   - "expected CRLF or '--' after boundary" (in part-data boundary tail)
 *   - "expected '--' at end of body"       (single '-' after closing boundary)
 *
 * http_parse_request swallows multipart parse failures silently — the
 * surface is "no files populated" or "no fields populated" rather than
 * a thrown exception. Each case asserts that the empty-result is what
 * we see, which is enough to drive the parser through the error arm. */

$bnd = '---bnd';

function probe(string $body, string $bnd): array {
    $req = "POST / HTTP/1.1\r\nHost: t\r\n"
         . "Content-Type: multipart/form-data; boundary=$bnd\r\n"
         . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
    $r = TrueAsync\http_parse_request($req);
    if ($r === false) return ['fail' => true];
    return ['files' => count($r->getFiles()), 'post' => $r->getPost()];
}

/* 1. After matching boundary, a junk byte (not CR / LF / '-') → ERROR. */
$body = "--$bnd?\r\n";
$r = probe($body, $bnd);
echo "junk-after-boundary: files=", $r['files'] ?? '?', "\n";

/* 2. CR not followed by LF after boundary. */
$body = "--$bnd\rXYZ\r\n";
$r = probe($body, $bnd);
echo "cr-without-lf: files=", $r['files'] ?? '?', "\n";

/* 3. CR/LF inside a header NAME (before ':'). */
$body = "--$bnd\r\nContent\rDisposition: form-data; name=\"x\"\r\n\r\nv\r\n--$bnd--\r\n";
$r = probe($body, $bnd);
echo "cr-in-header-name: files=", $r['files'] ?? '?', "\n";

/* 4. CR followed by junk inside header value tail. */
$body = "--$bnd\r\nContent-Disposition: form-data; name=\"x\"\rXYZ\r\n\r\nv\r\n--$bnd--\r\n";
$r = probe($body, $bnd);
echo "cr-in-header-value: files=", $r['files'] ?? '?', "\n";

/* 5. Boundary in part data followed by junk (not CRLF nor "--"): the
 *    "--$bnd?" pattern inside a body chunk. */
$body = "--$bnd\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\n"
      . "data\r\n--$bnd?\r\ntrailing\r\n--$bnd--\r\n";
$r = probe($body, $bnd);
echo "boundary-mid-data-junk: files=", $r['files'] ?? '?', "\n";

/* 6. Closing boundary with single '-' instead of '--'. */
$body = "--$bnd\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\nv\r\n--$bnd-X";
$r = probe($body, $bnd);
echo "single-dash-end: files=", $r['files'] ?? '?', "\n";

/* 7. Sanity control: a well-formed body parses cleanly. */
$body = "--$bnd\r\nContent-Disposition: form-data; name=\"ok\"\r\n\r\nv\r\n--$bnd--\r\n";
$r = probe($body, $bnd);
echo "control: post-ok=", $r['post']['ok'] ?? '<missing>', "\n";

echo "done\n";
?>
--EXPECT--
junk-after-boundary: files=?
cr-without-lf: files=?
cr-in-header-name: files=?
cr-in-header-value: files=?
boundary-mid-data-junk: files=?
single-dash-end: files=?
control: post-ok=v
done
