--TEST--
HttpServer: GET, POST and HEAD over HTTP/3 from curl --http3-only
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true]);
require __DIR__ . '/_h3_curl.inc';
if (!str_contains((string) shell_exec(escapeshellarg(h3_curl_bin()) . ' --version 2>&1'), 'HTTP3')) {
    die('skip curl without HTTP3');
}
?>
--FILE--
<?php
/* The h3client and aioquic harnesses are POSIX-only, so on Windows this is the
 * test that completes a QUIC handshake: curl runs as a child process, connects
 * over UDP and must negotiate h3 (`--http3-only` refuses a fallback). */

require_once __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/_h3_curl.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = sys_get_temp_dir() . '/h3-112-' . getmypid();
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!h3_gen_cert($key, $cert)) {
    echo "cert gen failed\n";
    exit(1);
}

$port = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* start() needs a TCP listener */
    ->addHttp3Listener('127.0.0.1', $port)
    ->setCertificate($cert)->setPrivateKey($key)
    ->setReadTimeout(5)->setWriteTimeout(5));
$server->addHttpHandler(function ($req, $res) {
    /* HTTP/3 may start the handler after the headers: wait for the body. */
    $len = strlen($req->awaitBody()->getBody());
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain')
        ->setBody($req->getMethod() . ' ' . $req->getUri() . ' ' . $len);
});

$client = spawn(function () use ($server, $port, $tmp) {
    $url = "https://127.0.0.1:$port";
    /* From a file: 70000 bytes do not fit a Windows command line. */
    $body = "$tmp/body.bin";
    file_put_contents($body, str_repeat('p', 70000));
    $base = ['-s', '-k', '--http3-only', '--max-time', '5',
             '-w', '\n%{http_code} h%{http_version}'];

    $runs = [
        'get'  => [...$base, "$url/a?x=1"],
        'post' => [...$base, '--data-binary', "@$body", "$url/p"],
        'head' => [...$base, '-I', "$url/h"],
    ];
    foreach ($runs as $label => $args) {
        [$rc, $out] = h3_curl($args);
        $out = str_replace("\r\n", "\n", $out);
        if ($label === 'head') {
            /* HEAD has no body: the header block and the -w line. */
            $out = preg_match('/^content-length: (\d+)$/mi', $out, $m)
                ? 'content-length ' . $m[1] . substr($out, strrpos($out, "\n"))
                : $out;
        }
        echo "$label rc=$rc: ", str_replace("\n", ' | ', trim($out)), "\n";
    }

    @unlink($body);
    $server->stop();
});

$server->start();
await($client);

@unlink($cert);
@unlink($key);
@rmdir($tmp);
echo "done\n";
?>
--EXPECT--
get rc=0: GET /a?x=1 0 | 200 h3
post rc=0: POST /p 70000 | 200 h3
head rc=0: content-length 9 | 200 h3
done
