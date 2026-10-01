--TEST--
HttpServerConfig: twelve methods whose value nothing read throw, naming what does the job (#393)
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Each tombstone throws on any argument, and the message names the mechanism
 * that does the job. A setter whose value nothing reads leaves its getter
 * misreporting the server: enableTls(true) does not make the constructor's
 * listener TLS, and isHttp2Enabled() reads false while the listener serves h2c. */

use TrueAsync\HttpServerConfig;

$calls = [
    'enableTls'                  => [[true], 'addListener($host, $port, true)'],
    'isTlsEnabled'               => [[], "getListeners() reports 'tls'"],
    'enableProtocolDetection'    => [[true], 'detection always runs'],
    'isProtocolDetectionEnabled' => [[], 'detection always runs'],
    'setWriteBufferSize'         => [[65536], 'setStreamWriteBufferBytes()'],
    'getWriteBufferSize'         => [[], 'setStreamWriteBufferBytes()'],
    'setAutoAwaitBody'           => [[true], 'call awaitBody() before getBody()'],
    'isAutoAwaitBodyEnabled'     => [[], 'call awaitBody() before getBody()'],
    'enableHttp2'                => [[false], 'addHttp2Listener()'],
    'isHttp2Enabled'             => [[], 'addHttp2Listener()'],
    'enableWebSocket'            => [[false], 'addWebSocketHandler()'],
    'isWebSocketEnabled'         => [[], 'addWebSocketHandler()'],
];

$config = new HttpServerConfig();

foreach ($calls as $method => [$args, $replacement]) {
    try {
        $config->$method(...$args);
        echo "$method: returned\n";
    } catch (Throwable $e) {
        echo "$method: ", $e::class,
            str_starts_with($e->getMessage(), "$method() is gone. ") ? ', gone' : ', MESSAGE',
            str_contains($e->getMessage(), $replacement) ? ', names the replacement' : ', NO REPLACEMENT',
            "\n";
    }
}
?>
--EXPECT--
enableTls: TrueAsync\HttpServerRuntimeException, gone, names the replacement
isTlsEnabled: TrueAsync\HttpServerRuntimeException, gone, names the replacement
enableProtocolDetection: TrueAsync\HttpServerRuntimeException, gone, names the replacement
isProtocolDetectionEnabled: TrueAsync\HttpServerRuntimeException, gone, names the replacement
setWriteBufferSize: TrueAsync\HttpServerRuntimeException, gone, names the replacement
getWriteBufferSize: TrueAsync\HttpServerRuntimeException, gone, names the replacement
setAutoAwaitBody: TrueAsync\HttpServerRuntimeException, gone, names the replacement
isAutoAwaitBodyEnabled: TrueAsync\HttpServerRuntimeException, gone, names the replacement
enableHttp2: TrueAsync\HttpServerRuntimeException, gone, names the replacement
isHttp2Enabled: TrueAsync\HttpServerRuntimeException, gone, names the replacement
enableWebSocket: TrueAsync\HttpServerRuntimeException, gone, names the replacement
isWebSocketEnabled: TrueAsync\HttpServerRuntimeException, gone, names the replacement
