--TEST--
Multipart: LF-only line endings + RFC 2046 preamble/epilogue + long-boundary edges
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* RFC 2046 §5.1.1 allows the parser to tolerate LF-only line endings
 * (some legacy / handwritten clients still emit them) and arbitrary
 * preamble/epilogue text around the part list. Exercises four parser
 * arms that the CRLF-only tests in 001-009 don't reach:
 *   1. LF-only after the boundary (BOUNDARY_ALMOST_DONE → LF branch)
 *   2. LF-only ending a header value
 *   3. Preamble text before the first boundary (MP_STATE_START skip)
 *   4. Epilogue text after the closing "--" (MP_STATE_END skip)
 *
 * Plus boundary-length edges: minimum (1 char) and near-maximum. */

function parse(string $req_str): array {
    $r = TrueAsync\http_parse_request($req_str);
    if ($r === false) return ['parsed' => false];
    return ['parsed' => true, 'files' => $r->getFiles(), 'post' => $r->getPost()];
}

/* ----- 1+2: LF-only terminators throughout the multipart body ----- */
$bnd = '---x';
$body = "--$bnd\n"
      . "Content-Disposition: form-data; name=\"lfname\"\n"
      . "\n"
      . "lf-value\n"
      . "--$bnd--\n";
$req = "POST / HTTP/1.1\r\nHost: t\r\n"
     . "Content-Type: multipart/form-data; boundary=$bnd\r\n"
     . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
$r = parse($req);
echo "lf-only: parsed=", $r['parsed'] ? 'yes' : 'no',
     " post-lfname=", $r['post']['lfname'] ?? '<missing>', "\n";

/* ----- 3: RFC 2046 preamble — bytes before the first boundary ----- */
$preamble = "This is a multipart message in MIME format.\r\nIgnore this preamble.\r\n";
$body = $preamble
      . "--$bnd\r\n"
      . "Content-Disposition: form-data; name=\"a\"\r\n\r\n"
      . "value-a\r\n"
      . "--$bnd--\r\n";
$req = "POST / HTTP/1.1\r\nHost: t\r\n"
     . "Content-Type: multipart/form-data; boundary=$bnd\r\n"
     . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
$r = parse($req);
echo "preamble: parsed=", $r['parsed'] ? 'yes' : 'no',
     " post-a=", $r['post']['a'] ?? '<missing>', "\n";

/* ----- 4: RFC 2046 epilogue — bytes after the closing boundary ----- */
$body = "--$bnd\r\n"
      . "Content-Disposition: form-data; name=\"b\"\r\n\r\n"
      . "value-b\r\n"
      . "--$bnd--\r\n"
      . "Trailing epilogue text that must be ignored.\r\n";
$req = "POST / HTTP/1.1\r\nHost: t\r\n"
     . "Content-Type: multipart/form-data; boundary=$bnd\r\n"
     . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
$r = parse($req);
echo "epilogue: parsed=", $r['parsed'] ? 'yes' : 'no',
     " post-b=", $r['post']['b'] ?? '<missing>', "\n";

/* ----- Single-char boundary (smallest legal) ----- */
$bnd1 = 'Y';
$body = "--$bnd1\r\n"
      . "Content-Disposition: form-data; name=\"s\"\r\n\r\n"
      . "short\r\n"
      . "--$bnd1--\r\n";
$req = "POST / HTTP/1.1\r\nHost: t\r\n"
     . "Content-Type: multipart/form-data; boundary=$bnd1\r\n"
     . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
$r = parse($req);
echo "one-char-boundary: parsed=", $r['parsed'] ? 'yes' : 'no',
     " post-s=", $r['post']['s'] ?? '<missing>', "\n";

/* ----- 60-char boundary (RFC max is 70; well within the parser's
 *       MULTIPART_MAX_BOUNDARY_LEN). ----- */
$bnd_long = str_repeat('Ab9-', 15);  // 60 chars
$body = "--$bnd_long\r\n"
      . "Content-Disposition: form-data; name=\"l\"\r\n\r\n"
      . "value-l\r\n"
      . "--$bnd_long--\r\n";
$req = "POST / HTTP/1.1\r\nHost: t\r\n"
     . "Content-Type: multipart/form-data; boundary=$bnd_long\r\n"
     . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
$r = parse($req);
echo "long-boundary: parsed=", $r['parsed'] ? 'yes' : 'no',
     " post-l=", $r['post']['l'] ?? '<missing>', "\n";

/* ----- Boundary-like prefix in body that almost-matches but diverges
 *       (exercises PART_DATA_BOUNDARY mismatch → flush_lookbehind). ----- */
$body = "--$bnd\r\n"
      . "Content-Disposition: form-data; name=\"trap\"\r\n\r\n"
      . "see what happens with --$bnd-but-not-quite inside the body\r\n"
      . "--$bnd--\r\n";
$req = "POST / HTTP/1.1\r\nHost: t\r\n"
     . "Content-Type: multipart/form-data; boundary=$bnd\r\n"
     . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
$r = parse($req);
echo "near-miss-boundary: parsed=", $r['parsed'] ? 'yes' : 'no',
     " trap-contains-but-not-quite=",
     (isset($r['post']['trap']) && str_contains($r['post']['trap'], 'but-not-quite')) ? 'yes' : 'no', "\n";

echo "done\n";
?>
--EXPECT--
lf-only: parsed=yes post-lfname=lf-value
preamble: parsed=yes post-a=value-a
epilogue: parsed=yes post-b=value-b
one-char-boundary: parsed=yes post-s=short
long-boundary: parsed=yes post-l=value-l
near-miss-boundary: parsed=yes trap-contains-but-not-quite=yes
done
