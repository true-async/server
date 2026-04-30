--TEST--
Multipart: getFile() method
--EXTENSIONS--
true_async_server
--FILE--
<?php
$body = "-----boundary\r\n" .
        "Content-Disposition: form-data; name=\"doc\"; filename=\"readme.txt\"\r\n" .
        "Content-Type: text/plain\r\n" .
        "\r\n" .
        "README\r\n" .
        "-----boundary--\r\n";

$request_str = "POST / HTTP/1.1\r\n" .
               "Host: test\r\n" .
               "Content-Type: multipart/form-data; boundary=---boundary\r\n" .
               "Content-Length: " . strlen($body) . "\r\n" .
               "\r\n" .
               $body;

$request = TrueAsync\http_parse_request($request_str);

// Get existing file
$file = $request->getFile('doc');
echo "Found: " . ($file !== null ? 'yes' : 'no') . "\n";
echo "Type: " . get_class($file) . "\n";

// Get non-existing file
$notFound = $request->getFile('nonexistent');
echo "Not found: " . ($notFound === null ? 'yes' : 'no') . "\n";
?>
--EXPECT--
Found: yes
Type: TrueAsync\UploadedFile
Not found: yes
