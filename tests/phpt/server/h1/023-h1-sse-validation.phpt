--TEST--
HttpResponse SSE API — input validation throws before the stream commits
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!shell_exec('which curl')) die('skip curl not installed');
?>
--FILE--
<?php
/* Every SSE input check runs BEFORE the response is switched into
 * streaming mode, so a rejected call leaves the response fully
 * buffered — provable by returning a normal body afterwards. One
 * buffered route exercises the whole validation surface in a single
 * curl round-trip. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19680 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $mark = function (callable $fn): string {
        try { $fn(); return 'no-throw'; }
        catch (Throwable $e) {
            $cls = $e::class;
            $short = substr($cls, strrpos($cls, '\\') + 1);
            $m = $e->getMessage();
            $kind =
                str_contains($m, 'CR or LF')   ? 'newline' :
               (str_contains($m, 'NUL')        ? 'nul'     :
               (str_contains($m, 'non-negative')? 'retry'  :
               (str_contains($m, 'non-SSE')    ? 'ct'      : 'other')));
            return "$short:$kind";
        }
    };

    $out = [];
    $out['event-lf']     = $mark(fn () => $res->sseEvent('ok', event: "a\nb"));
    $out['event-cr']     = $mark(fn () => $res->sseEvent('ok', event: "a\rb"));
    $out['id-lf']        = $mark(fn () => $res->sseEvent('ok', id: "a\nb"));
    $out['id-nul']       = $mark(fn () => $res->sseEvent('ok', id: "a\0b"));
    $out['retry-neg']    = $mark(fn () => $res->sseEvent('ok', retry: -1));
    $out['comment-lf']   = $mark(fn () => $res->sseComment("a\nb"));
    $out['sseRetry-neg'] = $mark(fn () => $res->sseRetry(-5));

    /* Conflicting Content-Type set up-front makes sseStart() throw. */
    $res->setHeader('Content-Type', 'application/json');
    $out['conflict-ct']  = $mark(fn () => $res->sseStart());

    /* None of the above committed the response — still buffered. */
    $out['committed'] = $res->isHeadersSent() ? 'yes' : 'no';

    $body = '';
    foreach ($out as $k => $v) { $body .= "$k=$v\n"; }

    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody($body);
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf('curl --http1.1 -s --max-time 3 http://127.0.0.1:%d/v', $port);
    echo shell_exec($cmd) ?? '';
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
event-lf=HttpServerInvalidArgumentException:newline
event-cr=HttpServerInvalidArgumentException:newline
id-lf=HttpServerInvalidArgumentException:newline
id-nul=HttpServerInvalidArgumentException:nul
retry-neg=HttpServerInvalidArgumentException:retry
comment-lf=HttpServerInvalidArgumentException:newline
sseRetry-neg=HttpServerInvalidArgumentException:retry
conflict-ct=HttpServerInvalidArgumentException:ct
committed=no
done
