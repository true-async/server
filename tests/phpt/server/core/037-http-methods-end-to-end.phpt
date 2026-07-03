--TEST--
HttpServer: all common HTTP methods (GET/HEAD/POST/PUT/PATCH/DELETE/OPTIONS)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Each method exercises a different switch arm in the llhttp method
 * decoder + the request->method handling on both protocol-handler and
 * Response paths. The 200/204/no-body branches inside emit_status_line
 * are also exercised by varying status codes. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) {
    $m = $req->getMethod();
    /* Different status per method to exercise more paths. */
    if ($m === 'OPTIONS') {
        $res->setStatusCode(204)
            ->setHeader('Allow', 'GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS')
            ->end();
        return;
    }
    if ($m === 'HEAD') {
        /* HEAD response: same headers as GET, no body. */
        $res->setStatusCode(200)
            ->setHeader('Content-Type', 'text/plain')
            ->setHeader('Content-Length', '5')
            ->end();
        return;
    }
    $res->setHeader('Content-Type', 'text/plain')
        ->setBody("$m " . $req->getUri())
        ->end();
});

$client = spawn(function() use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }
    $methods = [
        ['GET',     '/a', ''],
        ['HEAD',    '/b', ''],
        ['POST',    '/c', "x=1&y=2"],
        ['PUT',     '/d', "putbody"],
        ['PATCH',   '/e', "patchbody"],
        ['DELETE',  '/f', ''],
        ['OPTIONS', '/g', ''],
    ];
    foreach ($methods as [$m, $p, $body]) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
        stream_set_timeout($fp, 2);
        $req  = "$m $p HTTP/1.1\r\nHost: x\r\nConnection: close\r\n";
        if ($body !== '') {
            $req .= "Content-Length: " . strlen($body) . "\r\n\r\n" . $body;
        } else {
            $req .= "\r\n";
        }
        fwrite($fp, $req);
        $resp = '';
        while (!feof($fp)) {
            $c = fread($fp, 4096);
            if ($c === '' || $c === false) break;
            $resp .= $c;
        }
        fclose($fp);
        $he = strpos($resp, "\r\n\r\n");
        $head = $he === false ? $resp : substr($resp, 0, $he);
        $bd   = $he === false ? '' : substr($resp, $he + 4);
        $st   = explode("\r\n", $head)[0];
        echo "$m: ", str_replace('HTTP/1.1 ', '', $st),
             " body=", trim($bd) === '' ? '(empty)' : trim($bd), "\n";
    }
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
--EXPECT--
GET: 200 OK body=GET /a
HEAD: 200 OK body=(empty)
POST: 200 OK body=POST /c
PUT: 200 OK body=PUT /d
PATCH: 200 OK body=PATCH /e
DELETE: 200 OK body=DELETE /f
OPTIONS: 204 No Content body=(empty)
done
