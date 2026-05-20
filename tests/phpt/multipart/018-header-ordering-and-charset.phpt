--TEST--
Multipart: Content-Type-before-Content-Disposition + charset/duplicate headers
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Exercises src/formats/multipart_processor.c paths that the standard
 * RFC-canonical Content-Disposition-first orderings don't reach:
 *   - L327-328 on_header_field "process previous header = Content-Type"
 *     fires when CT appears before CD inside a part.
 *   - parse_content_type charset extraction when the part has
 *     `charset=` in Content-Type.
 *   - Multiple Content-Type headers in one part (later one wins). */

$b = '---bnd';
/* Case 1: Content-Type FIRST, then Content-Disposition. The processor
 * sees Content-Type's value finalised at the on_header_field transition
 * (the L327-328 dispatch arm) rather than at on_headers_complete. */
$part1 = "Content-Type: text/plain; charset=utf-8\r\n"
       . "Content-Disposition: form-data; name=\"a\"; filename=\"a.txt\"\r\n";
/* Case 2: regular ordering, sanity. */
$part2 = "Content-Disposition: form-data; name=\"b\"; filename=\"b.txt\"\r\n"
       . "Content-Type: application/json; charset=us-ascii\r\n";
/* Case 3: two Content-Type headers — second wins. */
$part3 = "Content-Disposition: form-data; name=\"c\"; filename=\"c.dat\"\r\n"
       . "Content-Type: text/plain\r\n"
       . "Content-Type: application/octet-stream\r\n";

$body = '';
foreach ([$part1, $part2, $part3] as $i => $hdrs) {
    $body .= "--$b\r\n" . $hdrs . "\r\n" . "data-$i\r\n";
}
$body .= "--$b--\r\n";

$req_str = "POST / HTTP/1.1\r\nHost: t\r\n"
         . "Content-Type: multipart/form-data; boundary=$b\r\n"
         . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;

$req = TrueAsync\http_parse_request($req_str);
$files = $req->getFiles();

foreach (['a', 'b', 'c'] as $k) {
    $f = $files[$k];
    echo "$k: name=", $f->getClientFilename(),
         " ct=",       var_export($f->getClientMediaType(),  true),
         " cs=",       var_export($f->getClientCharset(),    true), "\n";
}
echo "done\n";
?>
--EXPECT--
a: name=a.txt ct='text/plain' cs='utf-8'
b: name=b.txt ct='application/json' cs='us-ascii'
c: name=c.dat ct='application/octet-stream' cs=NULL
done
