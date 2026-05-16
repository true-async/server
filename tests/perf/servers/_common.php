<?php
/* Shared helpers for the single-worker perf-test servers.
 * Each server script accepts: <mode> <port>
 *   mode = h1 | h2c | h2tls
 */

use TrueAsync\HttpServerConfig;

function perf_parse_mode(array $argv): array
{
    if (count($argv) < 3) {
        fprintf(STDERR, "usage: %s <h1|h2c|h2tls> <port>\n", $argv[0]);
        exit(2);
    }
    $mode = $argv[1];
    $port = (int)$argv[2];
    if (!in_array($mode, ['h1', 'h2c', 'h2tls'], true)) {
        fprintf(STDERR, "bad mode: %s (expected h1|h2c|h2tls)\n", $mode);
        exit(2);
    }
    if ($port < 1 || $port > 65535) {
        fprintf(STDERR, "bad port: %d\n", $port);
        exit(2);
    }
    return [$mode, $port];
}

function perf_make_config(string $mode, int $port): HttpServerConfig
{
    $cfg = (new HttpServerConfig())
        ->setBacklog(512)
        ->setReadTimeout(30)
        ->setWriteTimeout(60);

    switch ($mode) {
        case 'h1':
            $cfg->addListener('127.0.0.1', $port);
            break;
        case 'h2c':
            $cfg->addHttp2Listener('127.0.0.1', $port, false);
            break;
        case 'h2tls':
            $certDir = __DIR__ . '/../certs';
            $cfg->addHttp2Listener('127.0.0.1', $port, true)
                ->setCertificate($certDir . '/cert.pem')
                ->setPrivateKey($certDir . '/key.pem');
            break;
    }

    return $cfg;
}
