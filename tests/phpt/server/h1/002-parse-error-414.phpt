--TEST--
HttpServer: oversized URI returns RFC-compliant 414 (PLAN_PARSE_ERRORS)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19817 + getmypid() % 1000;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

// Handler should NEVER be invoked — parse error fires before dispatch.
$server->addHttpHandler(function($request, $response) {
    $response->setStatusCode(200)->setBody('should-not-run')->end();
});

$client = spawn(function() use ($port, $server) {
    usleep(20000);

    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) {
        echo "connect failed: $errstr\n";
        $server->stop();
        return;
    }

    // 9 KB URI — exceeds HTTP_MAX_URI_SIZE (8 KB).
    $uri = '/' . str_repeat('A', 9216);
    $req = "GET $uri HTTP/1.1\r\nHost: localhost\r\n\r\n";
    fwrite($fp, $req);

    $response = '';
    stream_set_timeout($fp, 2);
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === '' || $chunk === false) break;
        $response .= $chunk;
    }
    fclose($fp);

    // Print just the status line + the Connection header presence.
    $lines = explode("\r\n", $response);
    echo "status: " . $lines[0] . "\n";
    echo "has-connection-close: " . (stripos($response, "\r\nConnection: close\r\n") !== false ? 'yes' : 'no') . "\n";

    $tel = $server->getTelemetry();
    echo "parse_errors_414_total=" . $tel['parse_errors_414_total'] . "\n";
    echo "parse_errors_4xx_total=" . $tel['parse_errors_4xx_total'] . "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECTF--
status: HTTP/1.1 414 URI Too Long
has-connection-close: yes
parse_errors_414_total=1
parse_errors_4xx_total=1
done
