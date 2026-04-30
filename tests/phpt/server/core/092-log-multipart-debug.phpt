--TEST--
HttpServer: multipart processor emits DEBUG records when log severity is DEBUG (PLAN_LOG.md Step 2)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\await;

$port = 19880 + getmypid() % 50;
$logfile = sys_get_temp_dir() . "/php-http-server-092-" . getmypid() . ".log";
@unlink($logfile);
$logfh = fopen($logfile, "w+b");

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSeverity(LogSeverity::DEBUG)
    ->setLogStream($logfh);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('ok')->end(); });

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) { $server->stop(); return; }

    $boundary = "----TestBoundary42";
    /* Use two headers per part so on_header_field's flush path fires
     * (it only triggers when a previous header had a value). */
    $body  = "--$boundary\r\n";
    $body .= "Content-Disposition: form-data; name=\"alpha\"\r\n";
    $body .= "Content-Type: text/plain\r\n\r\n";
    $body .= "value-a\r\n";
    $body .= "--$boundary\r\n";
    $body .= "Content-Disposition: form-data; name=\"beta\"\r\n";
    $body .= "Content-Type: text/plain\r\n\r\n";
    $body .= "value-b\r\n";
    $body .= "--$boundary--\r\n";

    $req  = "POST / HTTP/1.1\r\n";
    $req .= "Host: localhost\r\n";
    $req .= "Content-Type: multipart/form-data; boundary=$boundary\r\n";
    $req .= "Content-Length: " . strlen($body) . "\r\n";
    $req .= "Connection: close\r\n\r\n";
    $req .= $body;

    fwrite($fp, $req);
    stream_set_timeout($fp, 2);
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
    }
    fclose($fp);
    $server->stop();
});

$server->start();
await($client);

fflush($logfh);
fclose($logfh);
$log = file_get_contents($logfile);
@unlink($logfile);

echo "has DEBUG marker: ", (strpos($log, " DEBUG ") !== false ? "yes" : "no"), "\n";
echo "has multipart.header.flush: ", (strpos($log, "multipart.header.flush") !== false ? "yes" : "no"), "\n";
echo "has multipart.headers_complete: ", (strpos($log, "multipart.headers_complete") !== false ? "yes" : "no"), "\n";
echo "has alpha field record: ", (strpos($log, "field_name=alpha") !== false ? "yes" : "no"), "\n";
echo "has server.start: ", (strpos($log, "server.start") !== false ? "yes" : "no"), "\n";
echo "has server.stop: ", (strpos($log, "server.stop") !== false ? "yes" : "no"), "\n";

// Same flow but with INFO — multipart debug should NOT appear.
$logfile2 = sys_get_temp_dir() . "/php-http-server-092b-" . getmypid() . ".log";
@unlink($logfile2);
$logfh2 = fopen($logfile2, "w+b");
$config2 = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->setLogSeverity(LogSeverity::INFO)
    ->setLogStream($logfh2);
$server2 = new HttpServer($config2);
$server2->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('ok')->end(); });
$c2 = spawn(function () use ($port, $server2) {
    usleep(30000);
    $fp = @stream_socket_client("tcp://127.0.0.1:" . ($port + 1), $errno, $errstr, 2);
    if (!$fp) { $server2->stop(); return; }
    $boundary = "----X";
    $body = "--$boundary\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nv\r\n--$boundary--\r\n";
    $req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data; boundary=$boundary\r\n";
    $req .= "Content-Length: " . strlen($body) . "\r\nConnection: close\r\n\r\n" . $body;
    fwrite($fp, $req);
    stream_set_timeout($fp, 2);
    while (!feof($fp)) { if (!fread($fp, 8192)) break; }
    fclose($fp);
    $server2->stop();
});
$server2->start();
await($c2);
fflush($logfh2);
fclose($logfh2);
$log2 = file_get_contents($logfile2);
@unlink($logfile2);
echo "INFO has multipart debug: ", (strpos($log2, "multipart.header") !== false ? "yes" : "no"), "\n";
echo "INFO has server.start: ", (strpos($log2, "server.start") !== false ? "yes" : "no"), "\n";

echo "Done\n";
--EXPECT--
has DEBUG marker: yes
has multipart.header.flush: yes
has multipart.headers_complete: yes
has alpha field record: yes
has server.start: yes
has server.stop: yes
INFO has multipart debug: no
INFO has server.start: yes
Done
