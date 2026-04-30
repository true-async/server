--TEST--
HttpServer: HTTP/1 pipelining — multiple requests in one TCP write
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Regression guard for the former "lose pipelined tail bytes" bug.
 * Client writes N requests in a single fwrite() — typically delivered
 * as one TCP segment. The server must:
 *   1. answer all N (no drops),
 *   2. preserve request-to-response order (HTTP/1.1 invariant),
 *   3. echo each request's URI back, so we can verify both order and
 *      that no request body/header was corrupted by the shift. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19650 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function($req, $resp) {
    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'text/plain')
         ->setHeader('X-Uri', $req->getUri())
         ->setBody("uri=" . $req->getUri() . "\n");
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    $pass_total = 0;
    $fail_total = 0;

    foreach ([2, 5, 20] as $N) {
        $sock = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        if (!$sock) { echo "connect failed: $errstr\n"; continue; }
        stream_set_timeout($sock, 3);

        $req = '';
        for ($i = 1; $i <= $N; $i++) {
            $last = ($i === $N) ? "Connection: close\r\n" : "";
            $req .= "GET /r$i HTTP/1.1\r\nHost: x\r\n$last\r\n";
        }
        fwrite($sock, $req);

        $data = '';
        while (!feof($sock)) {
            $chunk = fread($sock, 8192);
            if ($chunk === false || $chunk === '') break;
            $data .= $chunk;
        }
        fclose($sock);

        $count = substr_count($data, "HTTP/1.1 200");
        preg_match_all('/x-uri:\s*(\S+)/i', $data, $m);
        $uris = $m[1];

        $expected = [];
        for ($i = 1; $i <= $N; $i++) $expected[] = "/r$i";

        $ok = ($count === $N && $uris === $expected);
        echo "N=$N: ", ($ok ? "ok" : "FAIL seen=$count uris=" . implode(',', $uris)), "\n";
        $ok ? $pass_total++ : $fail_total++;
    }

    echo "summary: pass=$pass_total fail=$fail_total\n";
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
N=2: ok
N=5: ok
N=20: ok
summary: pass=3 fail=0
Done
