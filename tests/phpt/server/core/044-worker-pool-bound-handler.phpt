--TEST--
HttpServer: an object-bound handler closure keeps its $this across the worker-pool fan-out
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
if (!exec('curl --version 2>/dev/null')) die('skip curl CLI not available');
?>
--FILE--
<?php
/* Regression: with setWorkers(N) > 1 the config + handler are replicated
 * to every worker thread via transfer_obj. A handler closure bound to an
 * object ($this) must keep that binding across the transfer.
 *
 * Before the closure-transfer fix in ext/async (closure_transfer_obj
 * snapshotted the closure without fci_cache.object), the worker rebuilt
 * an unbound closure and the first request dereferenced a NULL $this —
 * every worker died with SIGSEGV.
 *
 * setBootloader() defines the bound object's class inside each worker so
 * the transferred $this can be reconstructed there.
 *
 * See true-async/php-async: "preserve a closure's bound $this across
 * thread transfer". */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

$handlerClassSrc = <<<'PHP'
class BoundHandler {
    public string $tag = 'bound-this-survived';
    public function handle($req, $res): void {
        /* Reaches into $this — the binding must have survived the
         * cross-thread transfer or this is a NULL dereference. */
        $res->setStatusCode(200)->setBody($this->tag);
    }
}
PHP;

eval($handlerClassSrc);   // define BoundHandler in this (parent) thread

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$workers = 2;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers($workers)
    ->setBootloader(static function () use ($handlerClassSrc): void {
        if (!class_exists('BoundHandler', false)) {
            eval($handlerClassSrc);
        }
    });

$server  = new HttpServer($config);
$handler = new BoundHandler();
$server->addHttpHandler($handler->handle(...));   // closure bound to $handler

spawn(function () use ($port, $server) {
    /* Workers need a moment to thread up + bind. */
    usleep(400000);

    $ok = 0;
    for ($i = 0; $i < 16; $i++) {
        $out = (string) shell_exec(sprintf(
            'curl -s --max-time 2 http://127.0.0.1:%d/', $port));
        if ($out === 'bound-this-survived') {
            $ok++;
        }
    }

    echo "bound_handler_ok=", ($ok === 16 ? 1 : 0), "\n";
    echo "done\n";
    $server->stop();
});

$server->start();
?>
--EXPECTF--
bound_handler_ok=1
done
%A
