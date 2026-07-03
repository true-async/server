--TEST--
HttpResponse: getReasonPhrase covers every status code in http_status_reason switch
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* HttpResponse::getReasonPhrase() falls back to the static
 * http_status_reason() table when the user never called
 * setReasonPhrase(). Walks the entire switch in src/http_response.c so
 * every case line is exercised. The body of the 200 response carries a
 * JSON map of {status: phrase} so we can assert in one round-trip. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

/* The status table from src/http_response.c (canonical RFC 9110 set). */
$codes = [
    100 => 'Continue',
    101 => 'Switching Protocols',
    200 => 'OK',
    201 => 'Created',
    202 => 'Accepted',
    204 => 'No Content',
    206 => 'Partial Content',
    301 => 'Moved Permanently',
    302 => 'Found',
    303 => 'See Other',
    304 => 'Not Modified',
    307 => 'Temporary Redirect',
    308 => 'Permanent Redirect',
    400 => 'Bad Request',
    401 => 'Unauthorized',
    403 => 'Forbidden',
    404 => 'Not Found',
    405 => 'Method Not Allowed',
    408 => 'Request Timeout',
    409 => 'Conflict',
    410 => 'Gone',
    411 => 'Length Required',
    413 => 'Payload Too Large',
    414 => 'URI Too Long',
    415 => 'Unsupported Media Type',
    416 => 'Range Not Satisfiable',
    422 => 'Unprocessable Entity',
    429 => 'Too Many Requests',
    500 => 'Internal Server Error',
    501 => 'Not Implemented',
    502 => 'Bad Gateway',
    503 => 'Service Unavailable',
    504 => 'Gateway Timeout',
    /* Unknown bucket — exercises the default arm. */
    299 => 'Unknown',
];

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);

$server->addHttpHandler(function($req, $res) use ($codes) {
    $out = [];
    foreach (array_keys($codes) as $code) {
        $res->setStatusCode($code);
        $out[$code] = $res->getReasonPhrase();
    }
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/json')
        ->setBody(json_encode($out))->end();
});

$client = spawn(function() use ($port, $server, $codes) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    $resp = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $resp .= $c;
    }
    fclose($fp);
    $body = substr($resp, strpos($resp, "\r\n\r\n") + 4);
    $got = json_decode($body, true);
    $ok = 0; $bad = [];
    foreach ($codes as $code => $expected) {
        if (($got[$code] ?? null) === $expected) $ok++;
        else $bad[] = "$code: got=" . ($got[$code] ?? '<missing>') . " want=$expected";
    }
    echo "matches: $ok / ", count($codes), "\n";
    foreach ($bad as $b) echo "  $b\n";
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
--EXPECTF--
matches: %d / %d
done
