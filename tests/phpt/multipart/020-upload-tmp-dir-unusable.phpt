--TEST--
Multipart: an unusable upload_tmp_dir falls back without raising a diagnostic
--EXTENSIONS--
true_async_server
--INI--
upload_tmp_dir=/trueasync-upload-tmp-dir-that-does-not-exist
--FILE--
<?php
/* The parser runs in an event-loop callback with no PHP frame above it, so a diagnostic
 * raised while it works reaches a handler that has nothing to unwind to; Laravel installs
 * one that throws, and the request dies without a response. The handler below stands in for
 * it: reaching it at all is the failure this test guards. */
set_error_handler(static function (int $level, string $message): bool {
    echo "HANDLER: {$message}\n";

    return true;
});

$body = "-----boundary\r\n" .
        "Content-Disposition: form-data; name=\"myfile\"; filename=\"test.txt\"\r\n" .
        "Content-Type: text/plain\r\n" .
        "\r\n" .
        "Hello World!\r\n" .
        "-----boundary--\r\n";

$request_str = "POST /upload HTTP/1.1\r\n" .
               "Host: test\r\n" .
               "Content-Type: multipart/form-data; boundary=---boundary\r\n" .
               "Content-Length: " . strlen($body) . "\r\n" .
               "\r\n" .
               $body;

$request = TrueAsync\http_parse_request($request_str);
$file = $request->getFiles()['myfile'];

echo "Error: " . $file->getError() . "\n";
echo "Size: " . $file->getSize() . "\n";
?>
--EXPECT--
Error: 0
Size: 12
