--TEST--
Multipart: Mixed fields and files
--EXTENSIONS--
true_async_server
--FILE--
<?php
$body = "-----boundary\r\n" .
        "Content-Disposition: form-data; name=\"username\"\r\n" .
        "\r\n" .
        "john\r\n" .
        "-----boundary\r\n" .
        "Content-Disposition: form-data; name=\"email\"\r\n" .
        "\r\n" .
        "john@example.com\r\n" .
        "-----boundary\r\n" .
        "Content-Disposition: form-data; name=\"avatar\"; filename=\"photo.jpg\"\r\n" .
        "Content-Type: image/jpeg\r\n" .
        "\r\n" .
        "FAKE_JPEG_DATA\r\n" .
        "-----boundary--\r\n";

$request_str = "POST /register HTTP/1.1\r\n" .
               "Host: test\r\n" .
               "Content-Type: multipart/form-data; boundary=---boundary\r\n" .
               "Content-Length: " . strlen($body) . "\r\n" .
               "\r\n" .
               $body;

$request = TrueAsync\http_parse_request($request_str);

$post = $request->getPost();
echo "Fields: " . count($post) . "\n";
echo "username: " . $post['username'] . "\n";
echo "email: " . $post['email'] . "\n";

$files = $request->getFiles();
echo "Files: " . count($files) . "\n";
$avatar = $request->getFile('avatar');
echo "Avatar filename: " . $avatar->getClientFilename() . "\n";
echo "Avatar type: " . $avatar->getClientMediaType() . "\n";
?>
--EXPECT--
Fields: 2
username: john
email: john@example.com
Files: 1
Avatar filename: photo.jpg
Avatar type: image/jpeg
