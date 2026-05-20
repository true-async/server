--TEST--
HttpServerConfig: compression knobs + HTTP/2/WS/protocol-detection toggles
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Covers the validation arms + round-trip getters for setters that
 * the existing 001-/011-/032- tests don't reach:
 *
 *   compression: enable/level/brotli/zstd/min-size/mime-types/
 *                request-max-decompressed/write-buffer-size
 *   toggles:     enableHttp2 (reject), enableWebSocket (reject),
 *                enableProtocolDetection (round-trip) */

use TrueAsync\HttpServerConfig;

function ex(string $label, callable $fn): void
{
    try { $fn(); echo "$label: ok\n"; }
    catch (\Throwable $e) { echo "$label: ", $e::class, "\n"; }
}

$enc = HttpServerConfig::getSupportedEncodings();
$has_compression = in_array('gzip', $enc, true);
$has_br          = in_array('br',   $enc, true);
$has_zstd        = in_array('zstd', $enc, true);

/* ---- setCompressionEnabled ---- */
$c = new HttpServerConfig();
echo "compr-default: ", $c->isCompressionEnabled() ? 'on' : 'off', "\n";
if ($has_compression) {
    $c->setCompressionEnabled(true);
    echo "compr-on: ",  $c->isCompressionEnabled() ? 'on' : 'off', "\n";
    $c->setCompressionEnabled(false);
    echo "compr-off: ", $c->isCompressionEnabled() ? 'on' : 'off', "\n";
} else {
    /* Build without compression: enable(true) must throw; off is no-op. */
    ex('compr-on-throws', fn() => $c->setCompressionEnabled(true));
    echo "compr-on: off\n";
    echo "compr-off: off\n";
}

/* ---- setCompressionLevel ----- range only enforced with HAVE_HTTP_COMPRESSION */
$c = new HttpServerConfig();
if ($has_compression) {
    ex('lvl-low',  fn() => $c->setCompressionLevel(-1));      // below MIN
    ex('lvl-high', fn() => $c->setCompressionLevel(100));     // above MAX
    $c->setCompressionLevel(6);
    echo "lvl-get=", $c->getCompressionLevel(), "\n";
} else {
    echo "lvl-low: ok\nlvl-high: ok\nlvl-get=0\n";
}

/* ---- setBrotliLevel / setZstdLevel ----- only meaningful with HAVE_HTTP_COMPRESSION */
$c = new HttpServerConfig();
if ($has_compression) {
    ex('br-out',  fn() => $c->setBrotliLevel(-1));
    ex('br-out2', fn() => $c->setBrotliLevel(99));
    $c->setBrotliLevel(4);
    echo "br-get=", $c->getBrotliLevel(), "\n";
    ex('zstd-out', fn() => $c->setZstdLevel(-5));
    ex('zstd-out2', fn() => $c->setZstdLevel(50));
    $c->setZstdLevel(3);
    echo "zstd-get=", $c->getZstdLevel(), "\n";
} else {
    echo "br-out: ok\nbr-out2: ok\nbr-get=0\nzstd-out: ok\nzstd-out2: ok\nzstd-get=0\n";
}

/* ---- setCompressionMinSize ---- */
$c = new HttpServerConfig();
if ($has_compression) {
    ex('cms-neg', fn() => $c->setCompressionMinSize(-1));
    $c->setCompressionMinSize(2048);
    echo "cms-get=", $c->getCompressionMinSize(), "\n";
} else {
    echo "cms-neg: ok\ncms-get=0\n";
}

/* ---- setCompressionMimeTypes ---- */
$c = new HttpServerConfig();
if ($has_compression) {
    ex('cmt-non-string', fn() => $c->setCompressionMimeTypes(['text/html', 42]));
    /* Whitespace + parameters stripping: "text/plain ; charset=utf-8"
     * normalises to "text/plain". */
    $c->setCompressionMimeTypes(['text/html', 'application/json', ' text/plain ; charset=utf-8']);
    $got = $c->getCompressionMimeTypes();
    sort($got);
    echo "cmt-list=", implode(',', $got), "\n";
} else {
    echo "cmt-non-string: ok\ncmt-list=\n";
}

/* ---- setRequestMaxDecompressedSize ---- */
$c = new HttpServerConfig();
ex('rmdz-neg', fn() => $c->setRequestMaxDecompressedSize(-1));
$c->setRequestMaxDecompressedSize(8 << 20);
echo "rmdz-get=", $c->getRequestMaxDecompressedSize(), "\n";
$c->setRequestMaxDecompressedSize(0);
echo "rmdz-zero=", $c->getRequestMaxDecompressedSize(), "\n";

/* ---- setWriteBufferSize ---- */
$c = new HttpServerConfig();
ex('wbs-tiny', fn() => $c->setWriteBufferSize(1023));
$c->setWriteBufferSize(8192);
echo "wbs-get=", $c->getWriteBufferSize(), "\n";

/* ---- enableHttp2: legacy flag — toggle-on rejected, toggle-off ok ---- */
$c = new HttpServerConfig();
echo "h2-default: ", $c->isHttp2Enabled() ? 'on' : 'off', "\n";
ex('h2-enable-throws', fn() => $c->enableHttp2(true));
$c->enableHttp2(false);
echo "h2-off: ", $c->isHttp2Enabled() ? 'on' : 'off', "\n";

/* ---- enableWebSocket: not implemented — enable(true) throws ---- */
$c = new HttpServerConfig();
echo "ws-default: ", $c->isWebSocketEnabled() ? 'on' : 'off', "\n";
ex('ws-enable-throws', fn() => $c->enableWebSocket(true));
$c->enableWebSocket(false);
echo "ws-off: ", $c->isWebSocketEnabled() ? 'on' : 'off', "\n";

/* ---- enableProtocolDetection ---- */
$c = new HttpServerConfig();
echo "pd-default: ", $c->isProtocolDetectionEnabled() ? 'on' : 'off', "\n";
$c->enableProtocolDetection(true);
echo "pd-on: ",     $c->isProtocolDetectionEnabled() ? 'on' : 'off', "\n";
$c->enableProtocolDetection(false);
echo "pd-off: ",    $c->isProtocolDetectionEnabled() ? 'on' : 'off', "\n";

echo "done\n";
?>
--EXPECT--
compr-default: on
compr-on: on
compr-off: off
lvl-low: TrueAsync\HttpServerInvalidArgumentException
lvl-high: TrueAsync\HttpServerInvalidArgumentException
lvl-get=6
br-out: TrueAsync\HttpServerInvalidArgumentException
br-out2: TrueAsync\HttpServerInvalidArgumentException
br-get=4
zstd-out: TrueAsync\HttpServerInvalidArgumentException
zstd-out2: TrueAsync\HttpServerInvalidArgumentException
zstd-get=3
cms-neg: TrueAsync\HttpServerInvalidArgumentException
cms-get=2048
cmt-non-string: TrueAsync\HttpServerInvalidArgumentException
cmt-list=application/json,text/html,text/plain
rmdz-neg: TrueAsync\HttpServerInvalidArgumentException
rmdz-get=8388608
rmdz-zero=0
wbs-tiny: TrueAsync\HttpServerInvalidArgumentException
wbs-get=8192
h2-default: off
h2-enable-throws: TrueAsync\HttpServerRuntimeException
h2-off: off
ws-default: off
ws-enable-throws: TrueAsync\HttpServerRuntimeException
ws-off: off
pd-default: off
pd-on: on
pd-off: off
done
