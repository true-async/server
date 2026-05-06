--TEST--
HttpResponse::json() — array encode, string passthrough, status, flags, error path
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19900 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    /* Override default to a known value so the inheritance test below
     * has something deterministic to assert. */
    ->setJsonEncodeFlags(JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);

$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $resp) {
    switch ($req->getPath()) {
        case '/array':
            $resp->json(['ok' => true, 'msg' => 'привет/мир', 'n' => 42])->end();
            return;
        case '/passthrough':
            /* Caller hands us already-encoded JSON — server MUST NOT
             * re-encode (would produce a JSON string-of-a-string). */
            $resp->json('{"cached":1}')->end();
            return;
        case '/status':
            $resp->json(['error' => 'bad input'], 422)->end();
            return;
        case '/flags-pretty':
            /* Per-call $flags override the server default. */
            $resp->json(['a' => 1, 'b' => 2], 200, JSON_PRETTY_PRINT)->end();
            return;
        case '/throw-stripped':
            /* JSON_THROW_ON_ERROR is silently stripped — encode of a
             * resource fails silently into 500, no exception leaks. */
            $resource = fopen('php://memory', 'r');
            $resp->json(['r' => $resource], 200, JSON_THROW_ON_ERROR)->end();
            fclose($resource);
            return;
        case '/custom-ct':
            /* Handler-set Content-Type must NOT be overwritten. */
            $resp->setHeader('Content-Type', 'application/problem+json')
                 ->json(['type' => 'about:blank', 'title' => 'oops'], 400)
                 ->end();
            return;
    }
});

function fetch(int $port, string $path): array {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    $raw = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $raw .= $c;
    }
    fclose($fp);
    [$head, $body] = explode("\r\n\r\n", $raw, 2) + ['', ''];
    $lines = explode("\r\n", $head);
    $status = (int)(explode(' ', array_shift($lines))[1] ?? 0);
    $h = [];
    foreach ($lines as $l) {
        if (strpos($l, ':') === false) continue;
        [$k, $v] = explode(':', $l, 2);
        $h[strtolower(trim($k))] = trim($v);
    }
    return [$status, $h, $body];
}

$client = spawn(function () use ($port, $server) {
    delay(20);

    [$st, $h, $b] = fetch($port, '/array');
    echo "array status=$st ct=", $h['content-type'] ?? '<none>', " body=$b\n";

    [$st, , $b] = fetch($port, '/passthrough');
    echo "pass body=$b\n";

    [$st, , $b] = fetch($port, '/status');
    echo "status code=$st body=$b\n";

    [, , $b] = fetch($port, '/flags-pretty');
    /* Pretty-print emits embedded newlines + 4-space indents. */
    echo "pretty has-newline: ", (str_contains($b, "\n    ")) ? 1 : 0, "\n";

    [$st, , $b] = fetch($port, '/throw-stripped');
    echo "throw-stripped st=$st body=$b\n";

    [$st, $h, $b] = fetch($port, '/custom-ct');
    echo "custom-ct st=$st ct=", $h['content-type'] ?? '<none>', " body=$b\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
array status=200 ct=application/json body={"ok":true,"msg":"привет/мир","n":42}
pass body={"cached":1}
status code=422 body={"error":"bad input"}
pretty has-newline: 1
throw-stripped st=500 body={"error":"json encoding failed"}
custom-ct st=400 ct=application/problem+json body={"type":"about:blank","title":"oops"}
Done
