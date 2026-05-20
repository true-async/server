--TEST--
HttpServerConfig: argument validation rejects out-of-range values
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Exercises the validation arms on the most-used setters that don't
 * already have coverage. The drain-knob arms live in 011-drain-config-
 * validation; this file covers the rest of the value-range checks.
 * Each case constructs a fresh HttpServerConfig (so locked-state never
 * fires) and asserts whether the call threw or not. */

use TrueAsync\HttpServerConfig;

function check(string $label, callable $fn, bool $expectThrow): void
{
    $cfg = new HttpServerConfig();
    $threw = false;
    $msg = '';
    try {
        $fn($cfg);
    } catch (\Throwable $e) {
        $threw = true;
        $msg = $e->getMessage();
    }
    if ($threw !== $expectThrow) {
        printf("%s: FAIL threw=%d expect=%d msg=%s\n",
               $label, (int)$threw, (int)$expectThrow, $msg);
    } else {
        echo "$label: OK\n";
    }
}

/* ---- Port range (every listener-add method has its own check) ---- */
foreach (['addListener', 'addHttp1Listener', 'addHttp2Listener', 'addHttp3Listener'] as $m) {
    check("$m:port0",       fn(HttpServerConfig $c) => $c->$m('127.0.0.1',     0), true);
    check("$m:port65536",   fn(HttpServerConfig $c) => $c->$m('127.0.0.1', 65536), true);
    check("$m:port-valid",  fn(HttpServerConfig $c) => $c->$m('127.0.0.1', 12345), false);
}

/* ---- setBacklog (>= 1) ---- */
check('backlog:0',    fn(HttpServerConfig $c) => $c->setBacklog(0),    true);
check('backlog:-5',   fn(HttpServerConfig $c) => $c->setBacklog(-5),   true);
check('backlog:1',    fn(HttpServerConfig $c) => $c->setBacklog(1),    false);
check('backlog:8192', fn(HttpServerConfig $c) => $c->setBacklog(8192), false);

/* ---- setWorkers (1..1024) ---- */
check('workers:0',    fn(HttpServerConfig $c) => $c->setWorkers(0),    true);
check('workers:1025', fn(HttpServerConfig $c) => $c->setWorkers(1025), true);
check('workers:1',    fn(HttpServerConfig $c) => $c->setWorkers(1),    false);
check('workers:1024', fn(HttpServerConfig $c) => $c->setWorkers(1024), false);

/* ---- setMaxConnections (>= 0; 0 is allowed = unlimited) ---- */
check('maxconn:-1',   fn(HttpServerConfig $c) => $c->setMaxConnections(-1),   true);
check('maxconn:0',    fn(HttpServerConfig $c) => $c->setMaxConnections(0),    false);
check('maxconn:5000', fn(HttpServerConfig $c) => $c->setMaxConnections(5000), false);

/* ---- setMaxInflightRequests (>= 0) ---- */
check('inflight:-1',  fn(HttpServerConfig $c) => $c->setMaxInflightRequests(-1),  true);
check('inflight:0',   fn(HttpServerConfig $c) => $c->setMaxInflightRequests(0),   false);
check('inflight:500', fn(HttpServerConfig $c) => $c->setMaxInflightRequests(500), false);

/* ---- setStreamWriteBufferBytes (4096..64 MiB) ---- */
check('swb:4095',    fn(HttpServerConfig $c) => $c->setStreamWriteBufferBytes(4095),    true);
check('swb:67108865',fn(HttpServerConfig $c) => $c->setStreamWriteBufferBytes(67108865),true);
check('swb:4096',    fn(HttpServerConfig $c) => $c->setStreamWriteBufferBytes(4096),    false);
check('swb:1MiB',    fn(HttpServerConfig $c) => $c->setStreamWriteBufferBytes(1<<20),   false);

/* ---- setMaxBodySize (1024..16 GiB) ---- */
check('mbs:1023',  fn(HttpServerConfig $c) => $c->setMaxBodySize(1023),       true);
check('mbs:1024',  fn(HttpServerConfig $c) => $c->setMaxBodySize(1024),       false);
check('mbs:10MiB', fn(HttpServerConfig $c) => $c->setMaxBodySize(10 << 20),   false);

/* ---- HTTP/3 knobs ---- */
check('h3-idle:-1',     fn(HttpServerConfig $c) => $c->setHttp3IdleTimeoutMs(-1),                true);
check('h3-idle:0',      fn(HttpServerConfig $c) => $c->setHttp3IdleTimeoutMs(0),                 false);
check('h3-idle:30000',  fn(HttpServerConfig $c) => $c->setHttp3IdleTimeoutMs(30000),             false);

check('h3-win:1023',    fn(HttpServerConfig $c) => $c->setHttp3StreamWindowBytes(1023),          true);
check('h3-win:2GiB',    fn(HttpServerConfig $c) => $c->setHttp3StreamWindowBytes(2 << 30),       true);
check('h3-win:64KiB',   fn(HttpServerConfig $c) => $c->setHttp3StreamWindowBytes(65536),         false);

check('h3-streams:0',   fn(HttpServerConfig $c) => $c->setHttp3MaxConcurrentStreams(0),          true);
check('h3-streams:big', fn(HttpServerConfig $c) => $c->setHttp3MaxConcurrentStreams(1000001),    true);
check('h3-streams:128', fn(HttpServerConfig $c) => $c->setHttp3MaxConcurrentStreams(128),        false);

check('h3-budget:0',    fn(HttpServerConfig $c) => $c->setHttp3PeerConnectionBudget(0),          true);
check('h3-budget:4097', fn(HttpServerConfig $c) => $c->setHttp3PeerConnectionBudget(4097),       true);
check('h3-budget:32',   fn(HttpServerConfig $c) => $c->setHttp3PeerConnectionBudget(32),         false);

/* ---- JSON flags ---- */
check('json:negative',  fn(HttpServerConfig $c) => $c->setJsonEncodeFlags(-1),         true);
check('json:happy',     fn(HttpServerConfig $c) => $c->setJsonEncodeFlags(JSON_UNESCAPED_SLASHES), false);

/* ---- Round-trip getters (also covers the post-set value-readback paths) ---- */
$cfg = (new HttpServerConfig())
    ->setBacklog(512)
    ->setWorkers(4)
    ->setMaxConnections(2048)
    ->setMaxInflightRequests(8192)
    ->setStreamWriteBufferBytes(16384)
    ->setMaxBodySize(5 << 20)
    ->setHttp3IdleTimeoutMs(15000)
    ->setHttp3StreamWindowBytes(131072)
    ->setHttp3MaxConcurrentStreams(64)
    ->setHttp3PeerConnectionBudget(8)
    ->setJsonEncodeFlags(JSON_UNESCAPED_UNICODE);

echo "backlog=",         $cfg->getBacklog(),                       "\n";
echo "workers=",         $cfg->getWorkers(),                       "\n";
echo "maxconn=",         $cfg->getMaxConnections(),                "\n";
echo "inflight=",        $cfg->getMaxInflightRequests(),           "\n";
echo "swb=",             $cfg->getStreamWriteBufferBytes(),        "\n";
echo "mbs=",             $cfg->getMaxBodySize(),                   "\n";
echo "h3-idle=",         $cfg->getHttp3IdleTimeoutMs(),            "\n";
echo "h3-win=",          $cfg->getHttp3StreamWindowBytes(),        "\n";
echo "h3-streams=",      $cfg->getHttp3MaxConcurrentStreams(),     "\n";
echo "h3-budget=",       $cfg->getHttp3PeerConnectionBudget(),     "\n";
echo "json=",            $cfg->getJsonEncodeFlags(),               "\n";

/* getSupportedEncodings — list always contains identity, more if built in. */
$enc = HttpServerConfig::getSupportedEncodings();
echo "identity-listed=", in_array('identity', $enc, true) ? 'yes' : 'no', "\n";

/* addUnixListener — happy path + getListeners shape sanity. */
$u = (new HttpServerConfig())->addUnixListener('/tmp/foo.sock');
$l = $u->getListeners();
echo "unix-listener-type=", $l[0]['type'], " path=", $l[0]['path'], "\n";

echo "done\n";
?>
--EXPECT--
addListener:port0: OK
addListener:port65536: OK
addListener:port-valid: OK
addHttp1Listener:port0: OK
addHttp1Listener:port65536: OK
addHttp1Listener:port-valid: OK
addHttp2Listener:port0: OK
addHttp2Listener:port65536: OK
addHttp2Listener:port-valid: OK
addHttp3Listener:port0: OK
addHttp3Listener:port65536: OK
addHttp3Listener:port-valid: OK
backlog:0: OK
backlog:-5: OK
backlog:1: OK
backlog:8192: OK
workers:0: OK
workers:1025: OK
workers:1: OK
workers:1024: OK
maxconn:-1: OK
maxconn:0: OK
maxconn:5000: OK
inflight:-1: OK
inflight:0: OK
inflight:500: OK
swb:4095: OK
swb:67108865: OK
swb:4096: OK
swb:1MiB: OK
mbs:1023: OK
mbs:1024: OK
mbs:10MiB: OK
h3-idle:-1: OK
h3-idle:0: OK
h3-idle:30000: OK
h3-win:1023: OK
h3-win:2GiB: OK
h3-win:64KiB: OK
h3-streams:0: OK
h3-streams:big: OK
h3-streams:128: OK
h3-budget:0: OK
h3-budget:4097: OK
h3-budget:32: OK
json:negative: OK
json:happy: OK
backlog=512
workers=4
maxconn=2048
inflight=8192
swb=16384
mbs=5242880
h3-idle=15000
h3-win=131072
h3-streams=64
h3-budget=8
json=256
identity-listed=yes
unix-listener-type=unix path=/tmp/foo.sock
done
