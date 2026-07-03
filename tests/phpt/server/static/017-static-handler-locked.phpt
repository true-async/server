--TEST--
StaticHandler: locked-state — every setter throws after addStaticHandler()
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Exercises handler_check_locked()'s true branch on every setter.
 * The handler locks during HttpServer::addStaticHandler() (snapshot
 * is built and the descriptor's HTTP_STATIC_FLAG_LOCKED is set);
 * after that, all setters must throw HttpServerRuntimeException. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use TrueAsync\StaticDotfiles;
use TrueAsync\StaticSymlinks;

$root = sys_get_temp_dir() . '/sh-lock-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
register_shutdown_function(fn() => @rmdir($root));

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$sh     = new StaticHandler('/s/', $root);
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler($sh);

echo "isLocked-after-attach: ", $sh->isLocked() ? 'yes' : 'no', "\n";

function locked(string $label, callable $fn): void
{
    try {
        $fn();
        echo "$label: UNEXPECTED-OK\n";
    } catch (\TrueAsync\HttpServerRuntimeException $e) {
        echo "$label: LOCKED\n";
    } catch (\Throwable $e) {
        echo "$label: WRONG-TYPE ", $e::class, "\n";
    }
}

locked('setIndexFiles',        fn() => $sh->setIndexFiles('a.html'));
locked('disableIndex',         fn() => $sh->disableIndex());
locked('setOnMissing',         fn() => $sh->setOnMissing(StaticOnMissing::NEXT));
locked('enablePrecompressed',  fn() => $sh->enablePrecompressed('br'));
locked('disablePrecompressed', fn() => $sh->disablePrecompressed());
locked('setDotfilePolicy',     fn() => $sh->setDotfilePolicy(StaticDotfiles::ALLOW));
locked('setSymlinkPolicy',     fn() => $sh->setSymlinkPolicy(StaticSymlinks::FOLLOW));
locked('hide',                 fn() => $sh->hide('*.bak'));
locked('setEtagEnabled',       fn() => $sh->setEtagEnabled(false));
locked('setOpenFileCache',     fn() => $sh->setOpenFileCache(100, 30));
locked('disableOpenFileCache', fn() => $sh->disableOpenFileCache());
locked('setCacheControl',      fn() => $sh->setCacheControl('no-cache'));
locked('setHeader',            fn() => $sh->setHeader('X-Foo', 'bar'));
locked('setBrowseEnabled',     fn() => $sh->setBrowseEnabled(true));
locked('setMimeType',          fn() => $sh->setMimeType('txt', 'text/x-test'));

/* Getters remain unaffected by the lock. */
echo "getUrlPrefix:     ", $sh->getUrlPrefix(), "\n";
echo "getRootDirectory: ", $sh->getRootDirectory() === '' ? 'empty?' : 'set', "\n";

/* No actual requests — never call start(). Server descriptor must
 * still tear down cleanly even though it was never started. */
unset($server, $config, $sh);
echo "done\n";
?>
--EXPECT--
isLocked-after-attach: yes
setIndexFiles: LOCKED
disableIndex: LOCKED
setOnMissing: LOCKED
enablePrecompressed: LOCKED
disablePrecompressed: LOCKED
setDotfilePolicy: LOCKED
setSymlinkPolicy: LOCKED
hide: LOCKED
setEtagEnabled: LOCKED
setOpenFileCache: LOCKED
disableOpenFileCache: LOCKED
setCacheControl: LOCKED
setHeader: LOCKED
setBrowseEnabled: LOCKED
setMimeType: LOCKED
getUrlPrefix:     /s/
getRootDirectory: set
done
