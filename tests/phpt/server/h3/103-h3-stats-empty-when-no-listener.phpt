--TEST--
HttpServer: getHttp3Stats() returns [] when no H3 listener is configured
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServer')) die('skip http_server not loaded');
?>
--FILE--
<?php
/* getHttp3Stats is always present on the HttpServer class regardless of
 * whether --enable-http3 was set at build time. A server without any
 * addHttp3Listener call gets an empty array — this is the supported
 * "probe for H3 activity" pattern for telemetry collectors. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$c = (new HttpServerConfig())->addListener('127.0.0.1', 20200);
$s = new HttpServer($c);

$stats = $s->getHttp3Stats();
echo "is_array=", (int)is_array($stats), "\n";
echo "count=",    count($stats), "\n";
echo "done\n";
?>
--EXPECT--
is_array=1
count=0
done
