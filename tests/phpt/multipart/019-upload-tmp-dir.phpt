--TEST--
Multipart: uploads land in upload_tmp_dir
--EXTENSIONS--
true_async_server
--INI--
upload_tmp_dir=/tmp/trueasync-upload-tmp-dir-test
--FILE--
<?php
$dir = '/tmp/trueasync-upload-tmp-dir-test';
@mkdir($dir, 0777, true);

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

$stream = $file->getStream();
$path = stream_get_meta_data($stream)['uri'];
fclose($stream);

echo "Error: " . $file->getError() . "\n";
echo "In configured dir: " . (dirname($path) === $dir ? 'yes' : 'no') . "\n";

unset($file, $request);
@rmdir($dir);
?>
--EXPECT--
Error: 0
In configured dir: yes
