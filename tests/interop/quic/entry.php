<?php
/**
 * QUIC interop-runner endpoint — server role.
 *
 * Serves the files the runner drops in /www over HTTP/3 (UDP) plus HTTP/2 + 1.1
 * over TLS (TCP) on the same port, using the runner-provided certificate pair.
 * The client downloads files and the runner verifies their integrity, so a plain
 * static file server over a working H3 stack passes the transfer-class tests.
 *
 * Contract (https://github.com/quic-interop/quic-network-simulator):
 *   ROLE=server, TESTCASE, SSLKEYLOGFILE, QLOGDIR, certs in /certs, files in /www.
 * Unsupported test cases are screened (exit 127) in run_endpoint.sh before this
 * script ever starts, so here we just bring the server up.
 *
 * Env (overridable for the local smoke test in this directory's README):
 *   INTEROP_WWW   docroot                       (default /www)
 *   INTEROP_CERT  certificate chain (PEM)       (default /certs/cert.pem)
 *   INTEROP_KEY   private key (PEM)             (default /certs/priv.key)
 *   INTEROP_PORT  TCP+UDP port                  (default 443)
 *   WORKERS       worker threads                (default 1)
 */

declare(strict_types=1);

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpRequest;
use TrueAsync\HttpResponse;

$www     = getenv('INTEROP_WWW')  ?: '/www';
$cert    = getenv('INTEROP_CERT') ?: '/certs/cert.pem';
$key     = getenv('INTEROP_KEY')  ?: '/certs/priv.key';
$port    = (int) (getenv('INTEROP_PORT') ?: 443);
$workers = (int) (getenv('WORKERS') ?: 1);

if (!is_readable($cert) || !is_readable($key)) {
    fwrite(STDERR, "[interop] missing cert/key: {$cert} / {$key}\n");
    exit(1);
}

$config = new HttpServerConfig();
$config->setWorkers($workers);
$config->setMaxBodySize(64 * 1024 * 1024);

// HTTP/3 (QUIC, UDP) on the interop port — the path the runner's client uses.
// A plain TCP listener is required by start(); the interop client never connects
// to it. TLS — the cert AND the QUIC "h3" ALPN selector — is enabled at CONFIG
// level via enableTls(); the H3 listener inherits both from here. Configuring the
// cert on a per-listener TLS addListener() instead leaves the QUIC ALPN as the TCP
// list (h2/http1.1), so the server answers the client's "h3" with a fatal
// no_application_protocol alert and the handshake stalls.
$config->addListener('0.0.0.0', $port + 1);
$config->addHttp3Listener('0.0.0.0', $port);
$config->enableTls(true)->setCertificate($cert)->setPrivateKey($key);

// hq-interop (HTTP/0.9-over-QUIC): the runner negotiates this ALPN for the
// whole transport matrix (migration/rebinding/multiplexing/loss). The hq shim
// serves files straight off the transport reactor from this docroot — same
// files the h3 handler below serves, no PHP per request.
$config->setHttp3HqDocroot($www);

// INTEROP_DEBUG=1 turns on the ngtcp2 DEBUG bridge to stderr (one line per frame)
// for diagnosing handshake/transport failures under the interop simulator.
if (getenv('INTEROP_DEBUG')) {
    $config->setLogSeverity(\TrueAsync\LogSeverity::DEBUG);
    $config->setLogStream(fopen('php://stderr', 'w'));
}

$server = new HttpServer($config);

// The runner fetches files at the URL root (StaticHandler can't mount '/'), so map
// the request path onto the docroot and zero-copy it back with sendFile (handles
// Content-Length and Range, which the transfer/range test cases exercise). Reject
// traversal / NUL; anything that is not a readable file is a clean 404.
$root = realpath($www);

if ($root === false) {
    fwrite(STDERR, "[interop] docroot not found: {$www}\n");
    exit(1);
}

$server->addHttpHandler(static function (HttpRequest $req, HttpResponse $res) use ($root): void {
    $uri  = $req->getUri();
    $qpos = strpos($uri, '?');
    $path = rawurldecode($qpos === false ? $uri : substr($uri, 0, $qpos));

    if ($path === '' || $path[0] !== '/'
        || strpos($path, "\0") !== false || strpos($path, '..') !== false) {
        $res->setStatusCode(404)->setBody('not found');
        return;
    }

    $full = $root . $path;

    if (!is_file($full) || !is_readable($full)) {
        $res->setStatusCode(404)->setBody('not found');
        return;
    }

    $res->sendFile($full);
});

fprintf(STDERR, "[interop] server up :%d www=%s workers=%d testcase=%s\n",
    $port, $www, $workers, getenv('TESTCASE') ?: '-');

$server->start();
