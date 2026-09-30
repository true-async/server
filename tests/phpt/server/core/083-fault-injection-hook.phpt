--TEST--
HttpServer: libfiu fault points are enabled by name, once or until disabled
--EXTENSIONS--
true_async_server
--SKIPIF--
<?php
if (!function_exists('_http_fault_enable')) die('skip needs --enable-fault-injection');
?>
--FILE--
<?php
/* A known-answer check of the fault injection itself, before any test rests on
 * it: an enabled point fails, a one-time point fails exactly once, a sticky one
 * until it is disabled, an unknown point never, and the hit count says how
 * many failures went out. */

$p = 'test/known-answer';

var_dump(_http_fault_fail($p));

_http_fault_enable($p);
var_dump(_http_fault_fail($p), _http_fault_fail($p), _http_fault_hits($p));

_http_fault_enable($p, once: false);
var_dump(_http_fault_fail($p), _http_fault_fail($p), _http_fault_fail($p));
_http_fault_disable($p);
var_dump(_http_fault_fail($p), _http_fault_hits($p), _http_fault_hits('test/never-enabled'));

_http_fault_disable('test/never-enabled');

try {
    _http_fault_enable('');
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
bool(false)
bool(true)
bool(false)
int(1)
bool(true)
bool(true)
bool(true)
bool(false)
int(3)
int(0)
_http_fault_enable(): Argument #1 ($name) must be 1 to 63 bytes long
