--TEST--
Multipart: File upload
--EXTENSIONS--
true_async_server
--FILE--
<?php
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
$files = $request->getFiles();

echo "Count: " . count($files) . "\n";
$file = $files['myfile'];
echo "Filename: " . $file->getClientFilename() . "\n";
echo "Size: " . $file->getSize() . "\n";
echo "Type: " . $file->getClientMediaType() . "\n";
echo "Error: " . $file->getError() . "\n";
echo "isValid: " . ($file->isValid() ? 'yes' : 'no') . "\n";
echo "isReady: " . ($file->isReady() ? 'yes' : 'no') . "\n";
?>
--EXPECT--
Count: 1
Filename: test.txt
Size: 12
Type: text/plain
Error: 0
isValid: yes
isReady: yes
