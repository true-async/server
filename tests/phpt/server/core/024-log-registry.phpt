--TEST--
HttpServer log sink-type/formatter registry (#5, B5d): built-ins + dynamic error lists
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_log_registry_names')) die('skip test hooks not built');
?>
--FILE--
<?php
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;

$reg = _http_log_registry_names();
echo "types: {$reg['types']}\n";
echo "formatters: {$reg['formatters']}\n";

/* Unknown names are rejected with the registry-driven list in the message. */
foreach ([
    'bad-type'   => [['type' => 'gelf', 'level' => LogSeverity::INFO]],
    'bad-format' => [['type' => 'stdout', 'format' => 'xml', 'level' => LogSeverity::INFO]],
    /* syslog is a wire format, not a public formatter: it carries no record
     * framing, so on a plain stream the records would run together. */
    'syslog-fmt' => [['type' => 'stdout', 'format' => 'syslog', 'level' => LogSeverity::INFO]],
] as $tag => $spec) {
    try {
        (new HttpServerConfig())->setLogSinks($spec);
        echo "$tag: accepted\n";
    } catch (\Throwable $e) {
        echo "$tag: ", $e->getMessage(), "\n";
    }
}

echo "Done\n";
--EXPECT--
types: stream|file|stdout|stderr|syslog
formatters: plain|logfmt|json|pretty|template
bad-type: setLogSinks(): 'type' must be one of stream|file|stdout|stderr|syslog
bad-format: setLogSinks(): 'format' must be one of plain|logfmt|json|pretty|template
syslog-fmt: setLogSinks(): 'format' must be one of plain|logfmt|json|pretty|template
Done
