--TEST--
HttpResponse::sendFile() — basic 200 + Content-Type + body
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = tempnam(sys_get_temp_dir(), 'sf-');
rename($tmp, $tmp . '.css');
$tmp = $tmp . '.css';
file_put_contents($tmp, "body{margin:0}");

register_shutdown_function(function() use ($tmp) { @unlink($tmp); });

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($tmp) {
    $res->sendFile($tmp);
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
    $lines = explode("\r\n", $head);
    echo "status: ", $lines[0], "\n";
    foreach ($lines as $l) {
        if (stripos($l, 'content-type:') === 0) echo "ct: ", trim(substr($l, 13)), "\n";
        if (stripos($l, 'content-length:') === 0) echo "cl: ", trim(substr($l, 15)), "\n";
    }
    echo "body: ", $body, "\n";
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
--EXPECTF--
status: HTTP/1.1 200 OK
ct: text/css; charset=utf-8
cl: 14
body: body{margin:0}
done
