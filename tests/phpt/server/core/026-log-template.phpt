--TEST--
HttpServer template log formatter (#5): user-controlled line layout + {ts:...} date pattern
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_log_format_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

/* Goldens against the fixed canonical record (2024-01-01T00:00:00.123Z, INFO,
 * body "user login", 7 attrs, trace bytes 00..0f / span 00..07). */
$cases = [
    'date-pattern' => '{ts:Y-m-d H:i:s.v} [{level}] {msg}',
    'iso-default'  => '{ts}|{level}',
    'attrs-trace'  => '{msg}§{attrs}§{trace}§{span}',
    'unknown-lit'  => '{nope} {msg} {ts:y/m/d} tail{',
];
foreach ($cases as $tag => $tpl) {
    var_dump(_http_log_format_selftest('template', false, $tpl));
}

/* Invalid specs throw at config time. */
foreach ([
    'no-template'   => [['type' => 'stdout', 'format' => 'template',
                         'level' => LogSeverity::INFO]],
    'template-long' => [['type' => 'stdout', 'format' => 'template',
                         'template' => str_repeat('x', 300),
                         'level' => LogSeverity::INFO]],
] as $tag => $spec) {
    try {
        (new HttpServerConfig())->setLogSinks($spec);
        echo "$tag: accepted\n";
    } catch (\Throwable $e) {
        echo "$tag: rejected\n";
    }
}

/* End-to-end: a stream sink renders records through the compiled template. */
$port = tas_free_port_span(1);
$path = sys_get_temp_dir() . '/php-http-026-' . getmypid() . '.log';
@unlink($path);
$fh = fopen($path, 'w+b');

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $fh, 'format' => 'template',
         'template' => '>>{level}<< {msg}', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('OK')->end(); });
spawn(function () use ($server) { usleep(80000); $server->stop(); });
$server->start();

fflush($fh); fclose($fh);
$log = file_get_contents($path);
@unlink($path);

echo "e2e start: ", str_contains($log, '>>INFO<< server.start') ? "yes" : "no", "\n";
echo "e2e no-iso: ", str_contains($log, '2024-') || preg_match('/\dT\d/', $log) ? "leaked" : "yes", "\n";

echo "Done\n";
--EXPECT--
string(42) "2024-01-01 00:00:00.123 [INFO] user login
"
string(30) "2024-01-01T00:00:00.123Z|INFO
"
string(125) "user login§ path=/a b tag=v"1 line=a
b n=-7 sz=4294967296 ok=true r=1.5§000102030405060708090a0b0c0d0e0f§0001020304050607
"
string(33) "{nope} user login 24/01/01 tail{
"
no-template: rejected
template-long: rejected
e2e start: yes
e2e no-iso: yes
Done
