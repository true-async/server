--TEST--
HttpServer: the reactor-pool worker compresses responses, serves precompressed sendFile() sidecars, decodes request bodies and applies the JSON flags (#350)
--EXTENSIONS--
true_async_server
true_async
zlib
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* A worker builds the request and the response itself instead of going
 * through a transport's dispatch, so it has to wire compression the way the
 * transports do. Over the pool, with Accept-Encoding: gzip, a buffered and a
 * streamed text body come back gzip-encoded, with Vary and a Content-Length
 * that is the encoded size, and decode to what the handler wrote; a HEAD gets
 * no body; sendFile() serves the .gz sidecar, which the reactor picks by the
 * request's Accept-Encoding. A gzip-encoded POST reaches the handler decoded,
 * an unknown coding answers 415 and a corrupt gzip body 400. json() follows
 * setJsonEncodeFlags(). The same file passes with the pool off. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-078';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    foreach (['body.gz', 'bad.gz', 'stderr.txt', 'app.js', 'app.js.gz'] as $f) {
        @unlink("$tmp/$f");
    }
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$text = str_repeat("the quick brown fox jumps over the lazy dog\n", 800);
file_put_contents("$tmp/app.js", str_repeat("console.log('x');\n", 2000));
file_put_contents("$tmp/app.js.gz", gzencode(file_get_contents("$tmp/app.js")));

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setJsonEncodeFlags(JSON_UNESCAPED_SLASHES)
    ->setWorkers(2);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($text, $tmp) {
    switch ($req->getPath()) {
        case '/file':
            $res->sendFile("$tmp/app.js");
            break;

        case '/buffered':
            $res->setHeader('content-type', 'text/plain')->setBody($text);
            break;

        case '/stream':
            $res->setHeader('content-type', 'text/plain');
            foreach (str_split($text, 8192) as $chunk) {
                $res->write($chunk);
            }
            $res->end();
            break;

        case '/echo':
            $res->setHeader('content-type', 'text/plain')->setBody('len=' . strlen($req->getBody()) . ' '
                . ($req->getBody() === $text ? 'decoded' : 'NOT DECODED'));
            break;

        case '/json':
            $res->json(['path' => '/a/b']);
            break;
    }
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

function h3_request(string $bin, int $port, string $path, string $tmp, array $env,
                    string $method = 'GET', ?string $body_file = null): array
{
    $prefix = 'H3CLIENT_DEADLINE_MS=4000 H3CLIENT_PRINT_HEADERS=1';
    foreach ($env as $k => $v) {
        $prefix .= " $k=" . escapeshellarg($v);
    }

    $cmd = sprintf('%s %s 127.0.0.1 %d %s %s %s 2>%s', $prefix, escapeshellarg($bin), $port,
        escapeshellarg($path), $method, $body_file !== null ? escapeshellarg($body_file) : '',
        escapeshellarg("$tmp/stderr.txt"));
    $body = (string) shell_exec($cmd);
    $err = (string) @file_get_contents("$tmp/stderr.txt");

    preg_match('/^STATUS=(\d+)$/m', $err, $m);
    preg_match_all('/^HDR ([^:]+): (.*)$/m', $err, $h);

    return [(int) ($m[1] ?? -1), array_combine($h[1], $h[2]), $body];
}

spawn(function () use ($server, $port, $client_bin, $tmp, $text) {
    /* Reactors + workers need a moment to thread up and bind. */
    delay(600);

    $gzip = ['H3CLIENT_HEADER' => 'accept-encoding: gzip'];
    $expect = ['/buffered' => $text, '/stream' => $text, '/file' => file_get_contents("$tmp/app.js")];

    foreach ($expect as $path => $want) {
        [$status, $hdr, $body] = h3_request($client_bin, $port, $path, $tmp, $gzip);
        $ce = $hdr['content-encoding'] ?? 'identity';
        $plain = $ce === 'gzip' ? @gzdecode($body) : $body;
        echo "$path: status=$status content-encoding=$ce ",
            $plain === $want ? 'body intact' : 'body differs', ", ",
            strlen($body) < strlen($want) ? 'smaller' : 'not smaller', "\n";

        if ($path === '/buffered') {
            echo "/buffered: vary=", $hdr['vary'] ?? '-', " content-length ",
                ($hdr['content-length'] ?? '') === (string) strlen($body) ? 'matches' : 'differs', "\n";
        }
    }

    [$status, $hdr, $body] = h3_request($client_bin, $port, '/buffered', $tmp, $gzip, 'HEAD');
    echo "HEAD /buffered: status=$status body=", strlen($body), "\n";

    file_put_contents("$tmp/body.gz", gzencode($text));
    file_put_contents("$tmp/bad.gz", "\x1f\x8b\x08\x00garbage-not-deflate");

    foreach ([['gzip', 'body.gz'], ['bogus', 'body.gz'], ['gzip', 'bad.gz']] as [$coding, $file]) {
        [$status, , $body] = h3_request($client_bin, $port, '/echo', $tmp,
            ['H3CLIENT_HEADER' => "content-encoding: $coding"], 'POST', "$tmp/$file");
        echo "/echo $coding $file: status=$status", $status === 200 ? " $body" : '', "\n";
    }

    [$status, , $body] = h3_request($client_bin, $port, '/json', $tmp, []);
    echo "/json: status=$status $body\n";

    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
%A/buffered: status=200 content-encoding=gzip body intact, smaller
/buffered: vary=Accept-Encoding content-length matches
/stream: status=200 content-encoding=gzip body intact, smaller
/file: status=200 content-encoding=gzip body intact, smaller
HEAD /buffered: status=200 body=0
/echo gzip body.gz: status=200 len=35200 decoded
/echo bogus body.gz: status=415
/echo gzip bad.gz: status=400
/json: status=200 {"path":"/a/b"}
%Adone
