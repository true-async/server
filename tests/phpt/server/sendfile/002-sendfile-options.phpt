--TEST--
HttpResponse::sendFile() — options: contentType / disposition / cacheControl
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

$tmp = tempnam(sys_get_temp_dir(), 'sf-');
file_put_contents($tmp, "abcdef");

register_shutdown_function(function() use ($tmp) { @unlink($tmp); });

$port = 28320 + getmypid() % 1000;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($tmp) {
    $opts = new SendFileOptions(
        contentType:  'application/x-test',
        disposition:  SendFileDisposition::ATTACHMENT,
        downloadName: 'report.bin',
        cacheControl: 'public, max-age=3600',
    );
    $res->sendFile($tmp, $opts);
});

$client = spawn(function() use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    $resp = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $resp .= $c;
    }
    fclose($fp);
    $head_end = strpos($resp, "\r\n\r\n");
    $head = substr($resp, 0, $head_end);
    $body = substr($resp, $head_end + 4);
    $hd = [];
    foreach (explode("\r\n", $head) as $l) {
        if (str_contains($l, ':')) {
            [$k, $v] = explode(':', $l, 2);
            $hd[strtolower(trim($k))] = trim($v);
        }
    }
    echo "ct: ",   $hd['content-type']        ?? '?', "\n";
    echo "cd: ",   $hd['content-disposition'] ?? '?', "\n";
    echo "cc: ",   $hd['cache-control']       ?? '?', "\n";
    echo "body: ", $body, "\n";
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
--EXPECTF--
ct: application/x-test
cd: attachment; filename="report.bin"
cc: public, max-age=3600
body: abcdef
done
