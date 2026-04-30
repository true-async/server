--TEST--
Multipart: UploadedFile getStream()
--EXTENSIONS--
true_async_server
--FILE--
<?php
$body = "-----boundary\r\n" .
        "Content-Disposition: form-data; name=\"doc\"; filename=\"test.txt\"\r\n" .
        "Content-Type: text/plain\r\n" .
        "\r\n" .
        "Stream content\r\n" .
        "-----boundary--\r\n";

$request_str = "POST / HTTP/1.1\r\n" .
               "Host: test\r\n" .
               "Content-Type: multipart/form-data; boundary=---boundary\r\n" .
               "Content-Length: " . strlen($body) . "\r\n" .
               "\r\n" .
               $body;

$request = TrueAsync\http_parse_request($request_str);
$file = $request->getFile('doc');

$stream = $file->getStream();
echo "Is resource: " . (is_resource($stream) ? 'yes' : 'no') . "\n";
echo "Content: " . trim(stream_get_contents($stream)) . "\n";
fclose($stream);
?>
--EXPECT--
Is resource: yes
Content: Stream content
