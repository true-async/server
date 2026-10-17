--TEST--
Rate limit (#414): a Unix listener's peers share the key `unix` unless `unix` is a trusted proxy
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip AF_UNIX listener test is POSIX-only');
?>
--FILE--
<?php
/* A Unix-socket peer has no address. Without `unix` trusted every client of
 * the listener is the one key `unix`, so a peer rule still limits them, and
 * start() says so in the log; X-Forwarded-For is ignored. With `unix`
 * trusted the header names the client, and a request without one is `unix`. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/_rate_limit.inc';

function via_unix(string $path, ?string $forwarded): string
{
    $fp = stream_socket_client("unix://$path", $errno, $errstr, 2);
    stream_set_timeout($fp, 5);
    $out = rl_describe(rl_get($fp, '/', $forwarded === null ? [] : ['X-Forwarded-For' => $forwarded]));
    fclose($fp);
    return $out;
}

function run(bool $trust_unix, array $calls): void
{
    $path = sys_get_temp_dir() . '/tas-rl-094-' . getmypid() . ($trust_unix ? '-t' : '-u') . '.sock';
    @unlink($path);
    $log = fopen('php://temp', 'w+b');

    $config = (new HttpServerConfig())
        ->addUnixListener($path)
        ->setLogSeverity(LogSeverity::WARN)
        ->setLogStream($log)
        ->setReadTimeout(10)
        ->setWriteTimeout(10);

    if ($trust_unix) {
        $config->setTrustedProxies(['unix']);
    }

    $server = new HttpServer($config);
    $server->addRateLimit(new RateLimit(1, 60));
    $server->addHttpHandler(fn($req, $res) => $res->setBody('ok'));

    spawn(function () use ($server, $path, $calls) {
        delay(50);

        foreach ($calls as [$label, $forwarded]) {
            echo "  $label: ", via_unix($path, $forwarded), "\n";
        }

        $server->stop();
    });

    $server->start();
    rewind($log);
    $warned = str_contains(stream_get_contents($log), 'rate_limit.unix_peers_share_one_key');
    echo "  start() warned: ", $warned ? 'yes' : 'no', "\n";
    @unlink($path);
}

echo "unix not trusted:\n";
run(false, [['client A', '203.0.113.1'], ['client B', '203.0.113.2'], ['no header', null]]);

echo "unix trusted:\n";
run(true, [['client A', '203.0.113.1'], ['client B', '203.0.113.2'], ['client A again', '203.0.113.1'],
           ['no header', null], ['no header again', null]]);
echo "done\n";
--EXPECT--
unix not trusted:
  client A: 200 body=ok
  client B: 429 retry-after=60 body=Too Many Requests
  no header: 429 retry-after=60 body=Too Many Requests
  start() warned: yes
unix trusted:
  client A: 200 body=ok
  client B: 200 body=ok
  client A again: 429 retry-after=60 body=Too Many Requests
  no header: 200 body=ok
  no header again: 429 retry-after=60 body=Too Many Requests
  start() warned: no
done
