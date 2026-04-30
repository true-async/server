--TEST--
HttpResponse SSE API — sseStart / sseEvent / sseComment over HTTP/1.1
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

$port = 19940 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    /* Echo back Last-Event-ID so the client can verify request-side
     * surfacing without any extra plumbing. */
    $resume_from = $req->getHeader('Last-Event-ID');

    $res->sseStart();
    $res->sseComment("hello");                        /* keepalive */
    $res->sseEvent("alpha", id: "1");                 /* simple */
    $res->sseEvent("line1\nline2", event: "msg",      /* multiline */
                   id: "2", retry: 5000);
    $res->sseEvent("");                               /* empty payload */
    $res->sseEvent("resume=" . ($resume_from ?? "null"), id: "3");
    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http1.1 -i -s -N --max-time 3 -H "Last-Event-ID: prev-42" '
        . 'http://127.0.0.1:%d/events',
        $port
    );
    $out = []; exec($cmd, $out, $rc);

    $hdr_ct = $hdr_cc = $hdr_xab = $hdr_te = false;
    $body = [];
    $body_started = false;
    foreach ($out as $line) {
        if (!$body_started) {
            if (stripos($line, 'Content-Type:') === 0 &&
                stripos($line, 'text/event-stream') !== false) $hdr_ct = true;
            if (stripos($line, 'Cache-Control:') === 0 &&
                stripos($line, 'no-cache') !== false)         $hdr_cc = true;
            if (stripos($line, 'X-Accel-Buffering:') === 0 &&
                stripos($line, 'no') !== false)               $hdr_xab = true;
            if (stripos($line, 'Transfer-Encoding:') === 0 &&
                stripos($line, 'chunked') !== false)          $hdr_te = true;
            if ($line === '' || $line === "\r") $body_started = true;
        } else {
            $body[] = $line;
        }
    }
    /* Strip trailing \r curl leaves on each line. */
    $body = array_map(fn ($l) => rtrim($l, "\r"), $body);
    $joined = implode("\n", $body);

    echo "rc=$rc\n";
    echo "ct=", $hdr_ct?"y":"n", " cc=", $hdr_cc?"y":"n",
         " xab=", $hdr_xab?"y":"n", " te=", $hdr_te?"y":"n", "\n";

    /* Comment, in-order events, multiline data split, retry, resume value. */
    echo "comment=", (str_contains($joined, ": hello")             ? "y":"n"),
         " e1=",     (str_contains($joined, "id: 1\ndata: alpha")  ? "y":"n"),
         " e2_evt=", (str_contains($joined, "event: msg")          ? "y":"n"),
         " e2_ml=",  (str_contains($joined, "data: line1\ndata: line2")?"y":"n"),
         " retry=",  (str_contains($joined, "retry: 5000")         ? "y":"n"),
         " empty=",  (in_array("data: ", $body, true) || in_array("data:", $body, true) ?"y":"n"),
         " resume=", (str_contains($joined, "data: resume=prev-42")?"y":"n"),
         "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
rc=0
ct=y cc=y xab=y te=y
comment=y e1=y e2_evt=y e2_ml=y retry=y empty=y resume=y
done
