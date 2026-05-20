--TEST--
HttpResponse::sendFile() — RFC 5987 encoder triggered by non-ASCII downloadName
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\SendFileOptions;
use TrueAsync\SendFileDisposition;
use function Async\spawn;
use function Async\await;

$tmp = tempnam(sys_get_temp_dir(), 'sf5987-');
file_put_contents($tmp, "data");
register_shutdown_function(fn() => @unlink($tmp));

/* Three filenames that exercise distinct branches of http_rfc5987_encode:
 *   "ok.txt"      → ASCII-clean path: filename="ok.txt" (no encoding).
 *   "файл.txt"    → non-ASCII bytes → must be percent-encoded.
 *   "a\"b.txt"    → contains a double-quote → fails ASCII-clean check,
 *                   exercises encoder on a 7-bit char that is *not*
 *                   in the unreserved+attr-char set. */
$cases = [
    'a' => ['ok.txt',
            'attachment; filename="ok.txt"'],
    'b' => ['файл.txt',
            "attachment; filename*=UTF-8''%D1%84%D0%B0%D0%B9%D0%BB.txt"],
    'c' => ['a"b.txt',
            "attachment; filename*=UTF-8''a%22b.txt"],
];

$port   = 28430 + getmypid() % 1000;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($tmp, $cases) {
    /* Path is one char: /a /b /c — index into $cases. */
    $key = substr($req->getPath(), 1, 1);
    [$name, $_] = $cases[$key];
    $res->sendFile($tmp, new SendFileOptions(
        disposition:  SendFileDisposition::ATTACHMENT,
        downloadName: $name,
    ));
});

$client = spawn(function() use ($port, $server, $cases) {
    /* Wait for listener. */
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }
    foreach ($cases as $key => [$_, $want]) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
        stream_set_timeout($fp, 2);
        fwrite($fp, "GET /$key HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $resp = '';
        while (!feof($fp)) {
            $c = fread($fp, 4096);
            if ($c === '' || $c === false) break;
            $resp .= $c;
        }
        fclose($fp);
        $head = substr($resp, 0, strpos($resp, "\r\n\r\n"));
        $cd = '?';
        foreach (explode("\r\n", $head) as $l) {
            if (stripos($l, 'content-disposition:') === 0) {
                $cd = trim(substr($l, strlen('content-disposition:')));
            }
        }
        echo "$key: ", ($cd === $want ? "OK" : "MISMATCH got=[$cd] want=[$want]"), "\n";
    }
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
a: OK
b: OK
c: OK
done
