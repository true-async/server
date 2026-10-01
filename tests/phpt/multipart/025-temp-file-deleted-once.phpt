--TEST--
Multipart: an upload's temporary file is deleted once, by the UploadedFile that holds it
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* A temporary file name is free again once its file is gone, and on Windows
 * GetTempFileName hands the same few names to every process, so a second
 * delete of a path removes another upload's file. */

function parse_upload(): TrueAsync\HttpRequest
{
    $body = "--b\r\n"
          . "Content-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\n"
          . "\r\n"
          . "payload\r\n"
          . "--b--\r\n";

    return TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\n"
        . "Content-Type: multipart/form-data; boundary=b\r\n"
        . "Content-Length: " . strlen($body) . "\r\n\r\n" . $body);
}

function tmp_path_of(TrueAsync\UploadedFile $file): string
{
    $stream = $file->getStream();
    $path = stream_get_meta_data($stream)['uri'];
    fclose($stream);
    return $path;
}

/* A moved file's old path taken by a new file: the request's end leaves it. */
$request = parse_upload();
$file = $request->getFile('f');
$path = tmp_path_of($file);
$target = sys_get_temp_dir() . '/trueasync-025-' . getmypid() . '.txt';
$file->moveTo($target);
file_put_contents($path, 'another upload');
unset($file, $request);
echo "moved, path reused: ", file_exists($path) ? 'kept' : 'deleted', "\n";
@unlink($path);
@unlink($target);

/* An UploadedFile kept past its request still reads its file, and deletes it
 * when it goes. */
$request = parse_upload();
$file = $request->getFile('f');
$path = tmp_path_of($file);
unset($request);
echo "kept past the request: ", stream_get_contents($file->getStream() ?? fopen('php://memory', 'r')), "\n";
unset($file);
echo "after the object: ", file_exists($path) ? 'kept' : 'deleted', "\n";
?>
--EXPECT--
moved, path reused: kept
kept past the request: payload
after the object: deleted
