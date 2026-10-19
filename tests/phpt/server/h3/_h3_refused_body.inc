<?php
/* Shared body of the h3 tests for a request the server refuses or fails with a
 * status of its own (#412): start an HTTP/3 server with the given handler, run
 * one h3client request against it and return the status the client read.
 * Where the listener runs comes from TRUE_ASYNC_SERVER_REACTOR_POOL in --ENV--.
 *
 * Usage:
 *     require __DIR__ . '/_h3_refused_body.inc';
 *     echo 'STATUS=', h3_refused_status('104', $handler, ['body_bytes' => 200 * 1024]), "\n";
 *
 * Options:
 *   max_body    setMaxBodySize() value, none when absent;
 *   method      request method, POST when absent;
 *   path        request path, /refused when absent;
 *   body_bytes  size of the request body, no body when absent;
 *   body        the request body itself, instead of body_bytes;
 *   header      "name: value" for H3CLIENT_HEADER.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

require_once __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

/* @return int|null the status of the response, null when the client read none */
function h3_refused_status(string $tag, \Closure $handler, array $opt = []): ?int
{
    $tmp = __DIR__ . "/tmp-$tag-" . getmypid();
    mkdir($tmp, 0700);
    register_shutdown_function(function () use ($tmp) {
        foreach (glob("$tmp/*") as $f) {
            @unlink($f);
        }
        @rmdir($tmp);
    });

    if (!h3_gen_cert("$tmp/key.pem", "$tmp/cert.pem")) {
        echo "cert failed\n";
        return null;
    }

    $method = $opt['method'] ?? 'POST';
    $args   = '127.0.0.1 %d ' . escapeshellarg($opt['path'] ?? '/refused') . ' ' . $method;

    if (isset($opt['body']) || isset($opt['body_bytes'])) {
        file_put_contents("$tmp/body.bin", $opt['body'] ?? str_repeat('b', $opt['body_bytes']));
        $args .= ' ' . escapeshellarg("$tmp/body.bin");
    }

    $port = tas_free_port_span(2);
    $config = (new HttpServerConfig())
        ->addListener('127.0.0.1', $port + 1)
        ->addHttp3Listener('127.0.0.1', $port)
        ->setCertificate("$tmp/cert.pem")->setPrivateKey("$tmp/key.pem")
        ->setReadTimeout(10)->setWriteTimeout(10);

    if (isset($opt['max_body'])) {
        $config->setMaxBodySize($opt['max_body']);
    }

    if (getenv('TRUE_ASYNC_SERVER_REACTOR_POOL') === '1') {
        $config->setWorkers(2);
    }

    $server = new HttpServer($config);
    $server->addHttpHandler($handler);

    $status = null;

    spawn(function () use ($server, $port, $args, $opt, &$status) {
        delay(600);

        $env = 'H3CLIENT_DEADLINE_MS=4000';
        if (isset($opt['header'])) {
            $env .= ' H3CLIENT_HEADER=' . escapeshellarg($opt['header']);
        }

        $out = (string) shell_exec(sprintf('%s %s ' . $args . ' 2>&1',
            $env, escapeshellarg(__DIR__ . '/../../../h3client/h3client'), $port));

        if (preg_match('/^STATUS=(\d+)$/m', $out, $m)) {
            $status = (int) $m[1];
        } else {
            echo "client diagnostic: ", $out, "\n";
        }

        $server->stop();
    });

    $server->start();

    return $status;
}
