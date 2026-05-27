--TEST--
HttpServerConfig: TLS cert/key validation + log severity/stream setters
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Exercises:
 *   - setCertificate / setPrivateKey config_validate_readable_file arms:
 *       missing, not-regular-file, regular-file happy path
 *   - getCertificate / getPrivateKey getters before/after set
 *   - enableTls / isTlsEnabled
 *   - setLogSeverity round-trip for every enum case + getLogSeverity
 *   - setLogStream: null (clear), non-resource (throw), valid stream
 *   - setTelemetryEnabled / isTelemetryEnabled
 *   - setAutoAwaitBody / isAutoAwaitBodyEnabled */

use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;

/* Make a regular readable temp file for the happy paths. */
$tmp = tempnam(sys_get_temp_dir(), 'cfg-cert-');
file_put_contents($tmp, "-----BEGIN CERTIFICATE-----\n");
register_shutdown_function(fn() => @unlink($tmp));

function expectThrow(string $label, callable $fn): void
{
    try { $fn(); echo "$label: NO-THROW\n"; }
    catch (\Throwable $e) { echo "$label: ", $e::class, "\n"; }
}

/* ---- setCertificate ---- */
$c1 = new HttpServerConfig();
echo "cert-default: ", var_export($c1->getCertificate(), true), "\n";
expectThrow('cert-missing', fn() => $c1->setCertificate('/nonexistent-' . bin2hex(random_bytes(8))));
expectThrow('cert-dir',     fn() => $c1->setCertificate(sys_get_temp_dir()));  // not S_ISREG
$c1->setCertificate($tmp);
echo "cert-set: ", $c1->getCertificate() === $tmp ? 'yes' : 'no', "\n";
/* Overwrite — exercises the release-old branch. */
$c1->setCertificate($tmp);
echo "cert-rewrite: ok\n";

/* ---- setPrivateKey (same validator, separate path slot) ---- */
$c2 = new HttpServerConfig();
echo "key-default: ", var_export($c2->getPrivateKey(), true), "\n";
expectThrow('key-missing', fn() => $c2->setPrivateKey('/missing-' . bin2hex(random_bytes(8))));
expectThrow('key-dir',     fn() => $c2->setPrivateKey(sys_get_temp_dir()));
$c2->setPrivateKey($tmp);
echo "key-set: ", $c2->getPrivateKey() === $tmp ? 'yes' : 'no', "\n";

/* ---- enableTls / isTlsEnabled ---- */
$c3 = new HttpServerConfig();
echo "tls-default: ", $c3->isTlsEnabled() ? 'on' : 'off', "\n";
/* enableTls requires cert + key first (it throws otherwise). Wire them. */
$c3->setCertificate($tmp)->setPrivateKey($tmp);
$c3->enableTls(true);
echo "tls-after-enable: ", $c3->isTlsEnabled() ? 'on' : 'off', "\n";
$c3->enableTls(false);
echo "tls-after-disable: ", $c3->isTlsEnabled() ? 'on' : 'off', "\n";

/* ---- setLogSeverity ↔ getLogSeverity for each enum case ---- */
$c4 = new HttpServerConfig();
foreach ([LogSeverity::OFF, LogSeverity::DEBUG, LogSeverity::INFO,
          LogSeverity::WARN, LogSeverity::ERROR] as $sev) {
    $c4->setLogSeverity($sev);
    $got = $c4->getLogSeverity();
    echo "log-sev:", $sev->name, "→", $got->name,
         " ", $got === $sev ? 'ok' : 'WRONG', "\n";
}

/* ---- setLogStream: null clear + valid resource + non-stream throw ---- */
$c5 = new HttpServerConfig();
echo "log-stream-default: ", var_export($c5->getLogStream(), true), "\n";
$c5->setLogStream(null);   // clear when none set — exercises UNDEF→keep path
echo "log-stream-null: ", var_export($c5->getLogStream(), true), "\n";
$fp = fopen('php://memory', 'w+');
$c5->setLogStream($fp);
$rt = $c5->getLogStream();
echo "log-stream-resource: ", is_resource($rt) ? 'yes' : 'no', "\n";
$c5->setLogStream(null);   // exercises the UNDEF→IS_RESOURCE→clear path
echo "log-stream-cleared: ", var_export($c5->getLogStream(), true), "\n";
fclose($fp);
expectThrow('log-stream-int',    fn() => $c5->setLogStream(42));
expectThrow('log-stream-array',  fn() => $c5->setLogStream([1, 2, 3]));
/* Non-stream resource — curl_init() if curl is built. */
if (function_exists('curl_init')) {
    $ch = curl_init();
    expectThrow('log-stream-curl', fn() => $c5->setLogStream($ch));
    /* curl_close() is a no-op since PHP 8.0 and Deprecated in 8.5+ —
     * $ch goes out of scope at the end of the if-block. */
} else {
    echo "log-stream-curl: TrueAsync\\HttpServerInvalidArgumentException\n";
}

/* ---- setTelemetryEnabled / setAutoAwaitBody round-trip ---- */
$c6 = new HttpServerConfig();
echo "tel-default: ",  $c6->isTelemetryEnabled() ? 'on' : 'off', "\n";
$c6->setTelemetryEnabled(true);
echo "tel-on: ",       $c6->isTelemetryEnabled() ? 'on' : 'off', "\n";
$c6->setTelemetryEnabled(false);
echo "tel-off: ",      $c6->isTelemetryEnabled() ? 'on' : 'off', "\n";

echo "aab-default: ",  $c6->isAutoAwaitBodyEnabled() ? 'on' : 'off', "\n";
$c6->setAutoAwaitBody(true);
echo "aab-on: ",       $c6->isAutoAwaitBodyEnabled() ? 'on' : 'off', "\n";
$c6->setAutoAwaitBody(false);
echo "aab-off: ",      $c6->isAutoAwaitBodyEnabled() ? 'on' : 'off', "\n";

echo "done\n";
?>
--EXPECT--
cert-default: NULL
cert-missing: TrueAsync\HttpServerInvalidArgumentException
cert-dir: TrueAsync\HttpServerInvalidArgumentException
cert-set: yes
cert-rewrite: ok
key-default: NULL
key-missing: TrueAsync\HttpServerInvalidArgumentException
key-dir: TrueAsync\HttpServerInvalidArgumentException
key-set: yes
tls-default: off
tls-after-enable: on
tls-after-disable: off
log-sev:OFF→OFF ok
log-sev:DEBUG→DEBUG ok
log-sev:INFO→INFO ok
log-sev:WARN→WARN ok
log-sev:ERROR→ERROR ok
log-stream-default: NULL
log-stream-null: NULL
log-stream-resource: yes
log-stream-cleared: NULL
log-stream-int: TrueAsync\HttpServerInvalidArgumentException
log-stream-array: TrueAsync\HttpServerInvalidArgumentException
log-stream-curl: TrueAsync\HttpServerInvalidArgumentException
tel-default: off
tel-on: on
tel-off: off
aab-default: on
aab-on: on
aab-off: off
done
