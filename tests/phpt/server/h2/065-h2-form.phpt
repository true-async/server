--TEST--
HttpServer: HTTP/2 forms — url-encoded and multipart bodies reach getPost() and getFiles()
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* HTTP/2 starts the handler at the end of the headers, so the handler here
 * reads the form without awaitBody(): a form getter waits for the body itself.
 * The 1.2 MiB form is past the size at which body streaming takes a body over;
 * with streaming on it must still arrive buffered, or no getter could read it. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$upload = tempnam(sys_get_temp_dir(), 'h2form_');
file_put_contents($upload, 'file-bytes');
$form = tempnam(sys_get_temp_dir(), 'h2urlenc_');
/* A file, not an argument: escapeshellarg() on Windows turns % into a space. */
file_put_contents($form, 'a=1&list%5B%5D=2&list%5B%5D=3&map%5Bkey%5D=4');
$big = tempnam(sys_get_temp_dir(), 'h2big_');
file_put_contents($big, 'big=' . str_repeat('x', 1200 * 1024) . '&tail=end');

foreach ([false, true] as $streaming) {
    $port   = tas_free_port();
    $server = new HttpServer((new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setBodyStreamingEnabled($streaming)
        ->setReadTimeout(5)
        ->setWriteTimeout(5));

    $server->addHttpHandler(function ($req, $resp) {
        $post  = $req->getPost();
        $files = array_map(
            static fn($entry) => is_array($entry)
                ? array_map(static fn($file) => $file->getClientFilename() . ':' . $file->getSize(), $entry)
                : $entry->getClientFilename() . ':' . $entry->getSize(),
            $req->getFiles()
        );

        if (isset($post['big'])) {
            $post['big'] = strlen($post['big']);
        }

        $resp->setStatusCode(200)->setBody(json_encode(['post' => $post, 'files' => $files]));
    });

    $client = spawn(function () use ($port, $server, $upload, $form, $big, $streaming) {
        usleep(30000);
        /* Each argument quoted alone: cmd.exe knows no single quotes. */
        $curl = static fn(array $args): string => (string)shell_exec(sprintf(
            'curl --http2-prior-knowledge -sS --max-time 5 http://127.0.0.1:%d/form ', $port)
            . implode(' ', array_map('escapeshellarg', $args)) . ' 2>&1');

        echo 'streaming=', (int)$streaming, "\n";
        echo $curl(['--data-binary', "@$form"]), "\n";
        echo $curl(['-F', 'a=1', '-F', 'list[]=2', '-F', 'list[]=3', '-F', 'map[key]=4',
            '-F', "photos[]=@$upload;filename=one.txt", '-F', "docs[cv]=@$upload;filename=cv.txt"]), "\n";
        echo $curl(['--data-binary', "@$big"]), "\n";

        $server->stop();
    });

    $server->start();
    await($client);
}

@unlink($upload);
@unlink($form);
@unlink($big);
echo "Done\n";
?>
--EXPECT--
streaming=0
{"post":{"a":"1","list":["2","3"],"map":{"key":"4"}},"files":[]}
{"post":{"a":"1","list":["2","3"],"map":{"key":"4"}},"files":{"photos":["one.txt:10"],"docs":{"cv":"cv.txt:10"}}}
{"post":{"big":1228800,"tail":"end"},"files":[]}
streaming=1
{"post":{"a":"1","list":["2","3"],"map":{"key":"4"}},"files":[]}
{"post":{"a":"1","list":["2","3"],"map":{"key":"4"}},"files":{"photos":["one.txt:10"],"docs":{"cv":"cv.txt:10"}}}
{"post":{"big":1228800,"tail":"end"},"files":[]}
Done
