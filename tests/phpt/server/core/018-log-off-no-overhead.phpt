--TEST--
HttpServer: log-disabled hot path is free vs DEBUG (PLAN_LOG.md gate cost)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
/* Timing-sensitive perf gate: it compares wall-clock of a logging-off vs
 * a DEBUG-logging run, and the difference it measures is tens of
 * microseconds per request, which shared-CI jitter can swamp. Skip in CI
 * (GitHub sets GITHUB_ACTIONS automatically; SKIP_PERF_TESTS for other
 * CI); still runs locally where the machine is quiet. See #48. */
if (getenv('GITHUB_ACTIONS') !== false || getenv('SKIP_PERF_TESTS') !== false) {
    die('skip timing-sensitive perf gate — flaky under shared-CI jitter (#48)');
}
?>
--FILE--
<?php
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\await;

/* Send N multipart POSTs through the server, once with the logger
 * inactive (default — http_log_active == NULL, gate returns false in
 * one branch) and once with DEBUG severity routing to a file. The
 * DEBUG run pays vsnprintf + emalloc + ZEND_ASYNC_IO_WRITE per emit;
 * the OFF run skips them.
 *
 * One keep-alive connection carries every request. A fresh connect per
 * request costs about ten times what the logging does, and burying the
 * measured quantity under that noise is what made this comparison a coin
 * flip: measured over 1000 connects the ratio scattered from 0.96 to 1.04
 * across seven paired rounds, and over one connection it sits at 0.83–0.89,
 * seven rounds of seven below the threshold. */

const N = 2000;
const FIELDS = 8;

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
    $port = tas_free_port();
    $cfg = (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(15)
        ->setWriteTimeout(15);

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
    $body = gen_body($boundary, FIELDS);
    $head = "POST / HTTP/1.1\r\nHost: x\r\n"
          . "Content-Type: multipart/form-data; boundary=$boundary\r\n"
          . "Content-Length: " . strlen($body) . "\r\n\r\n";
    $req = $head . $body;

    $client = spawn(function () use ($port, $server, $req) {
        usleep(30000);
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        if (!$fp) { $server->stop(); return -1.0; }
        stream_set_timeout($fp, 5);

        $t0 = microtime(true);
        $answered = 0;
        for ($i = 0; $i < N; $i++) {
            fwrite($fp, $req);
            /* One request is in flight, so the first header terminator paces
             * the loop; the two-byte body trails inside the same read. */
            $reply = '';
            while (!str_contains($reply, "\r\n\r\n")) {
                $chunk = fread($fp, 4096);
                if ($chunk === false || $chunk === '') break 2;
                $reply .= $chunk;
            }

            $answered++;
        }
        $elapsed = microtime(true) - $t0;
        fclose($fp);
        $server->stop();

        /* A read that timed out or a server that went early leaves a number
         * that measures the truncation; it must not reach the comparison. */
        return $answered === N ? $elapsed : -1.0;
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
 * checked first). The margin it leaves is the 0.83–0.89 measured above,
 * which is what makes 1.0 a threshold rather than a coin toss. */
$ratio = $t_off / max($t_debug, 1e-6);
echo "off < debug: ", ($ratio < 1.0 ? "yes" : "no"), "\n";

echo "Done\n";
--EXPECT--
off run finite: yes
debug run finite: yes
off < debug: yes
Done
