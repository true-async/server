--TEST--
HttpServerConfig: setRequestScope — default on, toggle, chainable, locked guard, functional scope-off
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The per-request child scope is on by default: every request (and every
 * multiplexed H2/H3 stream) runs in its own async scope with an isolated
 * Async\request_context(). setRequestScope(false) reuses the connection
 * scope directly — saving two allocations per request, but then
 * request_context() returns null. The setter is chainable, defaults on,
 * and locks once the config is handed to a server. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

/* === Part A: default + toggle + chainable === */
$c = new HttpServerConfig();
echo "default=" . (int)$c->isRequestScope() . "\n";                              /* 1 */
echo "set false -> " . (int)$c->setRequestScope(false)->isRequestScope() . "\n"; /* 0 */
echo "set true  -> " . (int)$c->setRequestScope(true)->isRequestScope() . "\n";  /* 1 */
echo "chainable=" . (int)($c->setRequestScope(false) instanceof HttpServerConfig) . "\n"; /* 1 */

/* === Part B: locked after the config is handed to a server === */
$port = 19965 + getmypid() % 20;
$cfg = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setRequestScope(false)                /* serve with the per-request scope OFF */
    ->setReadTimeout(10)->setWriteTimeout(10);
$server = new HttpServer($cfg);
echo "effective=" . (int)$cfg->isRequestScope() . "\n";                          /* 0 */
try { $cfg->setRequestScope(true); echo "locked ACCEPTED\n"; }
catch (Throwable $e) { echo "locked rejected\n"; }

$server->addHttpHandler(function ($req, $res) {
    /* scope OFF ⇒ request_context() resolves to null (use ?->) */
    $ctx = \Async\request_context();
    $res->setStatusCode(200)->setBody(($ctx === null ? "ctx=null" : "ctx=set") . "\n");
});

/* === Part C: functional — three GETs on ONE keep-alive connection serve
 * correctly with the per-request scope disabled; each reports its
 * request_context() (null under scope-OFF), proving the handler runs and
 * consecutive keep-alive requests do not clobber one another. === */
$client = spawn(function () use ($port, $server) {
    usleep(150000);
    $url = sprintf('http://127.0.0.1:%d/', $port);
    $bodies = shell_exec(sprintf('curl -s %s %s %s 2>&1',
        escapeshellarg($url), escapeshellarg($url), escapeshellarg($url)));
    if ($server->isRunning()) {
        $server->stop();
    }

    return $bodies;
});

$server->start();
$bodies = await($client);
if ($server->isRunning()) {
    $server->stop();
}

$lines = array_values(array_filter(array_map('trim', explode("\n", (string)$bodies))));
echo "serve: " . count($lines) . "x " . (count(array_unique($lines)) === 1 ? ($lines[0] ?? '-') : "MIXED") . "\n";
echo "done\n";
?>
--EXPECT--
default=1
set false -> 0
set true  -> 1
chainable=1
effective=0
locked rejected
serve: 3x ctx=null
done
