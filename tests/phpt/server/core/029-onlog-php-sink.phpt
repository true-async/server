--TEST--
HttpServer onLog / 'php' sink (#5, B7): structured records to userland; exceptions absorbed
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(1);

$records = [];
$thrown  = 0;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->onLog(function (array $rec) use (&$records, &$thrown) {
        $records[] = $rec;
        if (count($records) === 1) {
            $thrown++;
            throw new RuntimeException('boom');   /* must be absorbed */
        }
    });

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(201)->setBody('yo')->end(); });

spawn(function () use ($server, $port) {
    usleep(50000);
    $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
    if ($c) {
        fwrite($c, "GET /cb HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        while (!feof($c)) { if (@fread($c, 8192) === false) break; }
        fclose($c);
    }
    usleep(30000);
    $server->stop();
});
$server->start();

echo "threw: ", $thrown, "\n";
echo "records: ", count($records) >= 2 ? "2+" : count($records), "\n";

$start  = null;
$access = null;
foreach ($records as $r) {
    if (str_starts_with($r['message'] ?? '', 'server.start')) $start = $r;
    if (($r['category'] ?? '') === 'access') $access = $r;
}
echo "start: ", $start !== null
    ? sprintf("cat=%s sev=%s", $start['category'], $start['severity_text'])
    : "missing", "\n";
echo "access: ", $access !== null
    ? sprintf("status=%d path=%s proto=%s", $access['attrs']['status'],
              $access['attrs']['path'], $access['attrs']['proto'])
    : "missing", "\n";

/* invalid callback rejected at config time */
try {
    (new HttpServerConfig())->setLogSinks([
        ['type' => 'php', 'callback' => 'no_such_fn_xyz', 'level' => LogSeverity::INFO]]);
    echo "bad-cb: accepted\n";
} catch (\Throwable $e) {
    echo "bad-cb: rejected\n";
}

echo "Done\n";
--EXPECT--
http_server log sink failed: onLog callback threw — exception absorbed, dropped=1
threw: 1
records: 2+
start: cat=app sev=INFO
access: status=201 path=/cb proto=h1
bad-cb: rejected
Done
