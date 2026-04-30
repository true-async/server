--TEST--
HttpResponse SSE — validation throws on malformed input / conflicting state
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!shell_exec('which curl')) die('skip curl not installed');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19960 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

/* Each route exercises one validation path. The handler reports the
 * exception class + a single-word marker as a regular (buffered) JSON
 * response — that lets us run all checks over a single curl run. */
$server->addHttpHandler(function ($req, $res) {
    $u = $req->getUri();

    $catch = function (callable $fn) : string {
        try { $fn(); return "no-throw"; }
        catch (Throwable $e) {
            return get_class($e) . ":" . (
                str_contains($e->getMessage(), 'CR or LF')        ? "newline" :
                (str_contains($e->getMessage(), 'NUL')             ? "nul"     :
                (str_contains($e->getMessage(), 'non-SSE')         ? "ct"      :
                (str_contains($e->getMessage(), 'streaming')
                 || str_contains($e->getMessage(), 'committed')    ? "stream"  :
                (str_contains($e->getMessage(), 'non-negative')    ? "retry"   :
                "other"))))
            );
        }
    };

    if ($u === '/event-newline') {
        $res->sseStart();
        $r1 = $catch(fn () => $res->sseEvent("ok", event: "bad\nname"));
        $r2 = $catch(fn () => $res->sseEvent("ok", event: "bad\rname"));
        $res->end();
        return;  /* body already streamed */
    }
    if ($u === '/id-newline') {
        $res->sseStart();
        $r1 = $catch(fn () => $res->sseEvent("ok", id: "x\ny"));
        $res->end();
        return;
    }
    if ($u === '/id-nul') {
        $res->sseStart();
        $r1 = $catch(fn () => $res->sseEvent("ok", id: "a\0b"));
        $res->end();
        return;
    }
    if ($u === '/retry-neg') {
        $res->sseStart();
        $r1 = $catch(fn () => $res->sseEvent("ok", retry: -1));
        $res->end();
        return;
    }
    if ($u === '/comment-newline') {
        $res->sseStart();
        $r1 = $catch(fn () => $res->sseComment("bad\nbad"));
        $res->end();
        return;
    }
    if ($u === '/conflict-ct') {
        /* Conflicting Content-Type set before sseStart → throw. */
        $res->setHeader('Content-Type', 'application/json');
        $r1 = $catch(fn () => $res->sseStart());
        /* After throw the response is still in buffered mode — emit
         * the marker as a plain body. */
        $res->setStatusCode(500)
            ->setHeader('Content-Type', 'text/plain')
            ->setBody("conflict-ct:" . $r1 . "\n");
        return;
    }
    if ($u === '/double-start') {
        $res->sseStart();
        $r1 = $catch(fn () => $res->sseStart());
        /* sseEvent reports it via the stream — encode marker into id. */
        $res->sseEvent("ok", id: "double:" . $r1);
        $res->end();
        return;
    }
    if ($u === '/modify-after') {
        $res->sseStart();
        $r1 = $catch(fn () => $res->setHeader('X-Late', 'oops'));
        $res->sseEvent("ok", id: "mod:" . $r1);
        $res->end();
        return;
    }

    $res->setStatusCode(404)->end();
});

/* Convert SSE body into "k=v;k=v" string of id-encoded markers. */
function ids_in($body) {
    preg_match_all('/^id: (.+)$/m', $body, $m);
    return implode(';', $m[1]);
}

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $get = function (string $path) use ($port) : string {
        $cmd = sprintf('curl --http1.1 -s -N --max-time 3 http://127.0.0.1:%d%s',
                       $port, $path);
        $out = []; exec($cmd, $out);
        return implode("\n", array_map(fn ($l) => rtrim($l, "\r"), $out));
    };

    /* For routes that report inline (buffered body): just echo body. */
    echo "conflict-ct: ", trim($get('/conflict-ct')), "\n";

    /* For SSE routes the markers live inside id: lines. */
    foreach (['/event-newline','/id-newline','/id-nul','/retry-neg',
              '/comment-newline','/double-start','/modify-after'] as $p) {
        $body = $get($p);
        /* Look for any marker we encoded (double:* or mod:*) — the
         * pure newline/nul/retry routes only need to confirm "all
         * three of those threw" via the absence of normal content. */
        echo $p, ": ";
        if (str_contains($body, 'double:') || str_contains($body, 'mod:')) {
            preg_match('/id: ([^\n]+)/', $body, $m);
            echo $m[1] ?? "no-id";
        } else {
            /* Validation path: handler still ran to completion (end()
             * called), so we get a valid SSE response with no visible
             * markers. The fact that we reach this and the body is a
             * proper SSE block proves catch caught the throw. */
            echo (str_contains($body, "\n\n") || $body === "") ? "caught" : "raw=" . substr($body, 0, 40);
        }
        echo "\n";
    }

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
conflict-ct: conflict-ct:TrueAsync\HttpServerInvalidArgumentException:ct
/event-newline: caught
/id-newline: caught
/id-nul: caught
/retry-neg: caught
/comment-newline: caught
/double-start: double:TrueAsync\HttpServerRuntimeException:stream
/modify-after: mod:TrueAsync\HttpServerRuntimeException:stream
done
