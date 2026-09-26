--TEST--
Multipart: field and file names nest by PHP's array rules
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* getPost() and getFiles() read field names the way PHP fills $_POST:
 * name[] appends, name[key] and name[0][1] nest, a dot in the base name
 * becomes an underscore. Files nest the same way, as UploadedFile objects. */

$boundary = 'bnd';
$parts    = [];

$field = static function (string $name, string $value) use (&$parts, $boundary): void {
    $parts[] = "--$boundary\r\nContent-Disposition: form-data; name=\"$name\"\r\n\r\n$value\r\n";
};

$file = static function (string $name, string $filename) use (&$parts, $boundary): void {
    $parts[] = "--$boundary\r\nContent-Disposition: form-data; name=\"$name\"; filename=\"$filename\"\r\n"
             . "Content-Type: text/plain\r\n\r\n$filename-bytes\r\n";
};

$field('plain', '1');
$field('list[]', 'a');
$field('list[]', 'b');
$field('map[key]', 'c');
$field('matrix[0][1]', 'd');
$field('dotted.name', 'e');
$file('photos[]', 'one.txt');
$file('photos[]', 'two.txt');
$file('docs[cv]', 'cv.txt');

$body    = implode('', $parts) . "--$boundary--\r\n";
$request = TrueAsync\http_parse_request(
    "POST /form HTTP/1.1\r\nHost: t\r\n"
    . "Content-Type: multipart/form-data; boundary=$boundary\r\n"
    . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body
);

var_dump($request->getPost());

$names = static fn(array $files): array => array_map(
    static fn($entry) => is_array($entry) ? array_map(static fn($f) => $f->getClientFilename(), $entry)
                                           : $entry->getClientFilename(),
    $files
);

var_dump($names($request->getFiles()));
echo 'getFile(photos): ', $request->getFile('photos')?->getClientFilename(), "\n";
echo 'getFile(docs): ', $request->getFile('docs')?->getClientFilename(), "\n";
?>
--EXPECT--
array(5) {
  ["plain"]=>
  string(1) "1"
  ["list"]=>
  array(2) {
    [0]=>
    string(1) "a"
    [1]=>
    string(1) "b"
  }
  ["map"]=>
  array(1) {
    ["key"]=>
    string(1) "c"
  }
  ["matrix"]=>
  array(1) {
    [0]=>
    array(1) {
      [1]=>
      string(1) "d"
    }
  }
  ["dotted_name"]=>
  string(1) "e"
}
array(2) {
  ["photos"]=>
  array(2) {
    [0]=>
    string(7) "one.txt"
    [1]=>
    string(7) "two.txt"
  }
  ["docs"]=>
  array(1) {
    ["cv"]=>
    string(6) "cv.txt"
  }
}
getFile(photos): one.txt
getFile(docs): cv.txt
