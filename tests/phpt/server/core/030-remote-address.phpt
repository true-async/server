--TEST--
HttpRequest::getRemoteAddress/getRemotePort (#5): bare IP + separate port
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(1);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $addr = $req->getRemoteAddress();
    $rport = $req->getRemotePort();

    /* Bare IP: no port glued on, no brackets. It must survive
     * FILTER_VALIDATE_IP, which "1.2.3.4:5678" would not. */
    $res->setStatusCode(200)->setBody(json_encode([
        'addr'     => $addr,
        'is_ip'    => filter_var($addr, FILTER_VALIDATE_IP) !== false,
        'no_colon' => !str_contains((string) $addr, ':'),
        'port_ok'  => is_int($rport) && $rport > 0 && $rport < 65536,
    ]))->end();
});

$out = null;
spawn(function () use ($server, $port, &$out) {
    usleep(50000);
    $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
    if ($c) {
        fwrite($c, "GET /peer HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $raw = '';
        while (!feof($c)) { $b = @fread($c, 8192); if ($b === false) break; $raw .= $b; }
        fclose($c);
        $out = substr($raw, strpos($raw, "\r\n\r\n") + 4);
    }
    usleep(50000);
    $server->stop();
});
$server->start();

$d = json_decode((string) $out, true);
echo "addr:     ", $d['addr'] ?? '?', "\n";
echo "is_ip:    ", ($d['is_ip'] ?? false) ? "yes" : "no", "\n";
echo "no_colon: ", ($d['no_colon'] ?? false) ? "yes" : "no", "\n";
echo "port_ok:  ", ($d['port_ok'] ?? false) ? "yes" : "no", "\n";
echo "Done\n";
?>
--EXPECT--
addr:     127.0.0.1
is_ip:    yes
no_colon: yes
port_ok:  yes
Done
