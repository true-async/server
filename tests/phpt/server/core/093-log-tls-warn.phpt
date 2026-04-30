--TEST--
HttpServer: TLS errors flow into the WARN log instead of php_error_docref (PLAN_LOG.md Step 3)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../tls/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/../tls/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-093';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

$port = 19940 + getmypid() % 50;
$logfile = sys_get_temp_dir() . "/php-http-server-093-" . getmypid() . ".log";
@unlink($logfile);
$logfh = fopen($logfile, "w+b");

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(3)
    ->setWriteTimeout(3)
    ->setLogSeverity(LogSeverity::WARN)
    ->setLogStream($logfh));

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('OK')->end(); });

$client = spawn(function () use ($port, $server) {
    usleep(80000);

    // Plain HTTP on TLS port — server's TLS handshake fails. Should
    // emit a tls.session.failed WARN record at teardown.
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if ($fp) {
        fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        $start = microtime(true);
        while (!feof($fp) && microtime(true) - $start < 1.5) {
            $r = @fread($fp, 8192);
            if ($r === false || $r === '') break;
        }
        fclose($fp);
    }
    usleep(100000);
    $server->stop();
});

$server->start();
await($client);

fflush($logfh);
fclose($logfh);
$log = file_get_contents($logfile);
@unlink($logfile);
@unlink($cert); @unlink($key); @rmdir($tmp);

echo "has WARN level: ", (strpos($log, " WARN ") !== false ? "yes" : "no"), "\n";
echo "has tls.session.failed: ", (strpos($log, "tls.session.failed") !== false ? "yes" : "no"), "\n";
echo "has ssl_err= field: ", (preg_match('/ssl_err=\d+/', $log) ? "yes" : "no"), "\n";
echo "Done\n";
--EXPECT--
has WARN level: yes
has tls.session.failed: yes
has ssl_err= field: yes
Done
