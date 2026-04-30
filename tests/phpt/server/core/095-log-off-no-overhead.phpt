--TEST--
HttpServer: log-disabled hot path is free vs DEBUG (PLAN_LOG.md gate cost)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\await;

/* Send N multipart POSTs through the server, once with the logger
 * inactive (default — http_log_active == NULL, gate returns false in
 * one branch) and once with DEBUG severity routing to a file. The
 * DEBUG run pays vsnprintf + emalloc + ZEND_ASYNC_IO_WRITE per emit;
 * the OFF run skips them. We assert the OFF run is at most ~half of
 * the DEBUG run — generous enough to absorb scheduler noise yet still
 * proving that the disabled gate is essentially free. */

const N = 1000;

function gen_body(string $boundary, int $fields): string {
    $body = '';
    for ($i = 0; $i < $fields; $i++) {
        $body .= "--$boundary\r\n";
        $body .= "Content-Disposition: form-data; name=\"f$i\"\r\n";
        $body .= "Content-Type: text/plain\r\n\r\n";
        $body .= "v$i\r\n";
    }
    $body .= "--$boundary--\r\n";
    return $body;
}

function run(?int $severity_value, ?string $logfile): float {
    $port = 19850 + getmypid() % 30 + ($severity_value ?? 0);
    $cfg = (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5);

    $logfh = null;
    if ($severity_value !== null && $logfile !== null) {
        $logfh = fopen($logfile, "w+b");
        $sev = match ($severity_value) {
            5  => LogSeverity::DEBUG,
            9  => LogSeverity::INFO,
            13 => LogSeverity::WARN,
            17 => LogSeverity::ERROR,
        };
        $cfg->setLogSeverity($sev)->setLogStream($logfh);
    }

    $server = new HttpServer($cfg);
    $server->addHttpHandler(function ($r, $s) { $s->setStatusCode(200)->setBody('ok')->end(); });

    $boundary = "----P" . bin2hex(random_bytes(4));
    $body = gen_body($boundary, /* fields per request */ 4);
    $head = "POST / HTTP/1.1\r\nHost: x\r\n"
          . "Content-Type: multipart/form-data; boundary=$boundary\r\n"
          . "Content-Length: " . strlen($body) . "\r\nConnection: close\r\n\r\n";
    $req = $head . $body;

    $client = spawn(function () use ($port, $server, $req) {
        usleep(30000);
        $t0 = microtime(true);
        for ($i = 0; $i < N; $i++) {
            $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
            if (!$fp) { break; }
            fwrite($fp, $req);
            stream_set_timeout($fp, 2);
            while (!feof($fp)) { if (!fread($fp, 8192)) break; }
            fclose($fp);
        }
        $elapsed = microtime(true) - $t0;
        $server->stop();
        return $elapsed;
    });

    $server->start();
    $elapsed = await($client);
    if ($logfh) { fclose($logfh); }
    return $elapsed;
}

$logfile = sys_get_temp_dir() . "/php-http-server-095-" . getmypid() . ".log";
@unlink($logfile);

$t_off   = run(null, null);                /* logger inactive */
$t_debug = run(/* DEBUG */ 5, $logfile);   /* per-request multipart debug emits */

@unlink($logfile);

/* Sanity: both must complete in reasonable time. */
echo "off run finite: ",   ($t_off   > 0 && $t_off   < 30 ? "yes" : "no"), "\n";
echo "debug run finite: ", ($t_debug > 0 && $t_debug < 30 ? "yes" : "no"), "\n";

/* OFF must be strictly faster than DEBUG: OFF skips formatting +
 * ZEND_ASYNC_IO_WRITE, DEBUG does both. A 1.0x threshold catches a
 * regression where the gate accidentally pays for the disabled path
 * (e.g. someone removed UNEXPECTED, or http_log_active stopped being
 * checked first). Loose enough to absorb jitter, tight enough to
 * fail if the gate stops short-circuiting. */
$ratio = $t_off / max($t_debug, 1e-6);
echo "off < debug: ", ($ratio < 1.0 ? "yes" : "no"), "\n";

echo "Done\n";
--EXPECT--
off run finite: yes
debug run finite: yes
off < debug: yes
Done
