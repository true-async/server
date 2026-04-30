--TEST--
HttpServer: admission reject on in-flight cap — H1 gets 503 + Retry-After
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Reproducer for PLAN_HTTP2 §Open follow-ups #3 H1 path.
 *
 * setMaxInflightRequests(2) + a handler that parks for 300ms. Fire
 * 6 concurrent keep-alive requests: the first 2 occupy the budget,
 * the next 4 arrive at on_headers_complete while active_requests == 2
 * and are short-circuited with 503 Service Unavailable + Retry-After.
 *
 * Verifies:
 *  - shed responses have correct status + Retry-After header,
 *  - the non-shed requests complete normally (200 OK),
 *  - requests_shed_total reflects the shed count,
 *  - server remains responsive after the burst. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19910 + getmypid() % 80;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxConnections(32)
    ->setMaxInflightRequests(2)   /* cap: 2 concurrent handlers */
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    delay(300);   /* hold the coroutine so the cap stays at 2 */
    $res->setStatusCode(200)->setBody('ok');
});

/* One client = one TCP connection, one request. 6 in parallel. */
$shoot = function (int $i) use ($port) {
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    if (!$fp) { return ['idx'=>$i,'status'=>'connect_fail']; }
    fwrite($fp, "GET /r$i HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    stream_set_timeout($fp, 3);
    $resp = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $resp .= $c;
    }
    fclose($fp);
    $status = explode("\r\n", $resp)[0] ?? '';
    $has_retry = stripos($resp, "\r\nRetry-After: ") !== false ? 'yes' : 'no';
    return ['idx'=>$i,'status'=>$status,'retry_after'=>$has_retry];
};

$driver = spawn(function () use ($port, $server, $shoot) {
    usleep(30000);

    /* Launch 6 clients concurrently (need at least 4 reject slots). */
    $tasks = [];
    for ($i = 0; $i < 6; $i++) {
        $tasks[] = spawn(fn() => $shoot($i));
    }
    $results = [];
    foreach ($tasks as $t) { $results[] = await($t); }

    /* Tally — ordering is non-deterministic, just count buckets. */
    $ok = 0; $shed = 0; $other = 0; $retry_after_on_503 = 0;
    foreach ($results as $r) {
        if     (stripos($r['status'], '200') !== false) { $ok++; }
        elseif (stripos($r['status'], '503') !== false) {
            $shed++;
            if ($r['retry_after'] === 'yes') { $retry_after_on_503++; }
        } else { $other++; }
    }
    echo "ok=$ok shed=$shed other=$other\n";
    echo "retry_after_on_all_shed=", ($retry_after_on_503 === $shed && $shed > 0 ? 'yes' : 'no'), "\n";
    echo "ok_ge_2=", ($ok >= 2 ? 'yes' : 'no'), "\n";
    echo "shed_ge_1=", ($shed >= 1 ? 'yes' : 'no'), "\n";

    $tel = $server->getTelemetry();
    echo "requests_shed_total_ge_shed=",
         ($tel['requests_shed_total'] >= $shed ? 'yes' : 'no'), "\n";
    echo "parse_errors_503_total_ge_shed=",
         ($tel['parse_errors_503_total'] >= $shed ? 'yes' : 'no'), "\n";

    /* Post-burst health check: single request must succeed once the
     * handlers drain below the cap (all 6 clients are done). */
    $post = $shoot(99);
    echo "post_burst_status: ", $post['status'], "\n";

    $server->stop();
});

$server->start();
await($driver);
echo "done\n";
--EXPECTF--
ok=%s shed=%s other=%s
retry_after_on_all_shed=yes
ok_ge_2=yes
shed_ge_1=yes
requests_shed_total_ge_shed=yes
parse_errors_503_total_ge_shed=yes
post_burst_status: HTTP/1.1 200%a
done
