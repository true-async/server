--TEST--
Multipart: UploadedFile moveTo()
--EXTENSIONS--
true_async_server
--FILE--
<?php
$body = "-----boundary\r\n" .
        "Content-Disposition: form-data; name=\"doc\"; filename=\"test.txt\"\r\n" .
        "Content-Type: text/plain\r\n" .
        "\r\n" .
        "File content here\r\n" .
        "-----boundary--\r\n";

$request_str = "POST / HTTP/1.1\r\n" .
               "Host: test\r\n" .
               "Content-Type: multipart/form-data; boundary=---boundary\r\n" .
               "Content-Length: " . strlen($body) . "\r\n" .
               "\r\n" .
               $body;

$request = TrueAsync\http_parse_request($request_str);
$file = $request->getFile('doc');

$target = sys_get_temp_dir() . '/test_upload_' . uniqid() . '.txt';
$file->moveTo($target);

echo "File exists: " . (file_exists($target) ? 'yes' : 'no') . "\n";
echo "Content: " . trim(file_get_contents($target)) . "\n";

// Try to move again - should throw
try {
    $file->moveTo($target . '.2');
    echo "Exception not thrown\n";
} catch (RuntimeException $e) {
    echo "Exception: " . $e->getMessage() . "\n";
}

// Cleanup
unlink($target);
?>
--EXPECT--
File exists: yes
Content: File content here
Exception: Cannot move file: file has already been moved
