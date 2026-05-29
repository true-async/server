<?php
/*
 * TLS conformance target — minimal HTTPS server for testssl.sh /
 * tlsfuzzer. Generates a throwaway self-signed cert (or reuses one
 * passed in env) and serves a generic 200 over TLS so the scanners
 * have a live endpoint to probe cipher suites / protocol versions /
 * known-vuln checks against.
 *
 * Usage: php tls_conformance_server.php [port] [cert.pem] [key.pem]
 * Env:   TLS_CONF_CERT / TLS_CONF_KEY override the cert/key paths.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$port = (int)($argv[1] ?? 18443);
$cert = $argv[2] ?? getenv('TLS_CONF_CERT') ?: sys_get_temp_dir() . '/tls_conf_cert.pem';
$key  = $argv[3] ?? getenv('TLS_CONF_KEY')  ?: sys_get_temp_dir() . '/tls_conf_key.pem';

if (!is_file($cert) || !is_file($key)) {
    $cmd = sprintf(
        'openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
        . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
        escapeshellarg($key), escapeshellarg($cert));
    exec($cmd, $out, $rc);
    if ($rc !== 0 || !is_file($cert) || !is_file($key)) {
        fwrite(STDERR, "cert generation failed\n");
        exit(1);
    }
}

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setKeepAliveTimeout(30)
    ->setMaxConnections(0);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody("ok\n");
});

$server->start();
