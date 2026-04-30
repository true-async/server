--TEST--
HttpServer: peer RST during awaitBody() wakes handler with HttpException(499)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* Step 7.2a — suspended-on-awaitBody variant of 076. When a client
 * aborts mid-upload, the handler coroutine is parked in the
 * body_event wait; cancel must wake it with HttpException(499)
 * instead of leaving it suspended forever. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19831 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);

$caught_code      = 0;
$caught_msg       = '';
$handler_finished = false;

$server->addHttpHandler(function ($req, $res)
    use (&$caught_code, &$caught_msg, &$handler_finished) {
    try {
        $req->awaitBody();
        $handler_finished = true;
        $res->setStatusCode(200)->setBody('unexpected');
    } catch (HttpException $e) {
        $caught_code = $e->getCode();
        $caught_msg  = $e->getMessage();
    }
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* 10 MB body + 0.3 s deadline + --limit-rate 500K — throttles
     * curl's upload to ~500 KB/s, so a 10 MB body would take ~20 s.
     * curl hits --max-time at 0.3 s and aborts mid-upload. The
     * server is suspended in awaitBody when the abort propagates as
     * RST_STREAM / FIN. --limit-rate makes the race independent of
     * localhost bandwidth (otherwise Step 9's eager-SETTINGS
     * handshake + ~500 MB/s localhost lets even 100 MB complete
     * before --max-time). */
    /* 10 MiB of zeroes via tempfile + curl --data-binary @file —
     * `head -c N /dev/zero | curl --data-binary @-` is bash-only and
     * neither `head` nor `/dev/zero` exists on Windows. Tempfile is
     * cross-platform and mirrors what curl does internally anyway. */
    $payload_tmp = tempnam(sys_get_temp_dir(), 'p77');
    $fh = fopen($payload_tmp, 'wb');
    $chunk = str_repeat("\0", 65536);
    for ($i = 0; $i < 160; $i++) { fwrite($fh, $chunk); }  /* 160 * 64K = 10 MiB */
    fclose($fh);
    $devnull = PHP_OS_FAMILY === 'Windows' ? 'nul' : '/dev/null';
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -o %s --max-time 0.3 '
        . '--limit-rate 500K '
        . '-X POST --data-binary @%s -H "Expect:" http://127.0.0.1:%d/upload',
        $devnull, escapeshellarg($payload_tmp), $port
    );
    $out = [];
    exec($cmd, $out, $rc);
    @unlink($payload_tmp);
    echo "curl_rc_is_timeout=", (int)($rc === 28), "\n";

    /* Let the cancel + dispose unwind. */
    delay(200);

    $server->stop();
});

$server->start();
await($client);

echo "handler_finished=", (int)$handler_finished, "\n";
echo "caught_code=$caught_code\n";
echo "caught_msg=$caught_msg\n";
echo "done\n";
--EXPECT--
curl_rc_is_timeout=1
handler_finished=0
caught_code=499
caught_msg=stream reset by peer
done
