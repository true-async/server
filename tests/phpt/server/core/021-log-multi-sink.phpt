--TEST--
HttpServer setLogSinks (#5, B4): multiple sinks, per-sink format + level, invalid spec
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
$pid  = getmypid();
$mk = function (string $tag) use ($pid) {
    $p = sys_get_temp_dir() . "/php-http-021-$tag-$pid.log";
    @unlink($p);
    return [$p, fopen($p, "w+b")];
};

[$jsonPath, $jsonFh]     = $mk('json');    // DEBUG → gets INFO start/stop
[$logfmtPath, $logfmtFh] = $mk('logfmt');  // INFO  → gets INFO start/stop
[$prettyPath, $prettyFh] = $mk('pretty');  // ERROR → filters INFO out

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $jsonFh,   'format' => 'json',   'level' => LogSeverity::DEBUG],
        ['type' => 'stream', 'stream' => $logfmtFh, 'format' => 'logfmt', 'level' => LogSeverity::INFO],
        ['type' => 'stream', 'stream' => $prettyFh, 'format' => 'pretty', 'level' => LogSeverity::ERROR],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('OK')->end(); });

spawn(function () use ($server) { usleep(80000); $server->stop(); });
$server->start();

foreach ([$jsonFh, $logfmtFh, $prettyFh] as $fh) { fflush($fh); fclose($fh); }
$json   = file_get_contents($jsonPath);
$logfmt = file_get_contents($logfmtPath);
$pretty = file_get_contents($prettyPath);
@unlink($jsonPath); @unlink($logfmtPath); @unlink($prettyPath);

/* json sink: valid JSON lines, one carries the server.start Body. */
$startSeen = false;
foreach (explode("\n", trim($json)) as $line) {
    if ($line === '') continue;
    $rec = json_decode($line, true);
    if (is_array($rec) && str_starts_with($rec['Body'] ?? '', 'server.start')) {
        $startSeen = true;
    }
}
echo "json start: ", $startSeen ? "yes" : "no", "\n";

/* logfmt sink: the same record fanned out here in logfmt (quoted msg). */
echo "logfmt start: ", str_contains($logfmt, 'msg="server.start') ? "yes" : "no", "\n";
echo "logfmt stop: ",  str_contains($logfmt, 'server.stop') ? "yes" : "no", "\n";

/* pretty sink at ERROR: INFO records filtered → empty. */
echo "pretty size: ", strlen(trim($pretty)), "\n";

/* invalid specs throw at config time. */
foreach ([
    'bad-type'    => [['type' => 'socket', 'level' => LogSeverity::INFO]],
    'no-level'    => [['type' => 'stdout', 'format' => 'json']],
    'bad-format'  => [['type' => 'stdout', 'format' => 'xml', 'level' => LogSeverity::INFO]],
    'stream-noderes' => [['type' => 'stream', 'level' => LogSeverity::INFO]],
] as $tag => $spec) {
    try {
        (new HttpServerConfig())->setLogSinks($spec);
        echo "$tag: accepted\n";
    } catch (\Throwable $e) {
        echo "$tag: rejected\n";
    }
}

echo "Done\n";
--EXPECT--
json start: yes
logfmt start: yes
logfmt stop: yes
pretty size: 0
bad-type: rejected
no-level: rejected
bad-format: rejected
stream-noderes: rejected
Done
