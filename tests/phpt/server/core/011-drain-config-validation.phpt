--TEST--
HttpServerConfig: drain-knob validation rejects misconfig
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Step 8 — each drain setter has a lower bound that catches
 * sub-second / sub-100ms misconfig at config-time (see
 * docs/PLAN_CONN_DRAIN.md §10 and validation in
 * src/http_server_config.c). Negatives + too-small values throw
 * InvalidArgumentException so they surface during dev rather than
 * waiting to fail in production. */

use TrueAsync\HttpServerConfig;

$cases = [
    /* Age: 0 (off) ok, >=1000 ok, else reject */
    ['setMaxConnectionAgeMs',       -1,     true],
    ['setMaxConnectionAgeMs',       500,    true],
    ['setMaxConnectionAgeMs',       0,      false],
    ['setMaxConnectionAgeMs',       1000,   false],

    /* Grace: 0 (infinite) ok, >=1000 ok, else reject */
    ['setMaxConnectionAgeGraceMs',  -1,     true],
    ['setMaxConnectionAgeGraceMs',  999,    true],
    ['setMaxConnectionAgeGraceMs',  0,      false],
    ['setMaxConnectionAgeGraceMs',  30000,  false],

    /* Spread: must be >= 100 */
    ['setDrainSpreadMs',            99,     true],
    ['setDrainSpreadMs',            100,    false],
    ['setDrainSpreadMs',            5000,   false],

    /* Cooldown: must be >= 1000 */
    ['setDrainCooldownMs',          999,    true],
    ['setDrainCooldownMs',          1000,   false],
];

foreach ($cases as [$method, $arg, $expectThrow]) {
    $cfg = new HttpServerConfig();
    $threw = false;
    try {
        $cfg->$method($arg);
    } catch (\Throwable $e) {
        $threw = true;
    }
    $ok = ($threw === $expectThrow) ? 'ok' : 'FAIL';
    echo sprintf("%s(%d) threw=%d expect=%d %s\n",
        $method, $arg, (int)$threw, (int)$expectThrow, $ok);
}
--EXPECT--
setMaxConnectionAgeMs(-1) threw=1 expect=1 ok
setMaxConnectionAgeMs(500) threw=1 expect=1 ok
setMaxConnectionAgeMs(0) threw=0 expect=0 ok
setMaxConnectionAgeMs(1000) threw=0 expect=0 ok
setMaxConnectionAgeGraceMs(-1) threw=1 expect=1 ok
setMaxConnectionAgeGraceMs(999) threw=1 expect=1 ok
setMaxConnectionAgeGraceMs(0) threw=0 expect=0 ok
setMaxConnectionAgeGraceMs(30000) threw=0 expect=0 ok
setDrainSpreadMs(99) threw=1 expect=1 ok
setDrainSpreadMs(100) threw=0 expect=0 ok
setDrainSpreadMs(5000) threw=0 expect=0 ok
setDrainCooldownMs(999) threw=1 expect=1 ok
setDrainCooldownMs(1000) threw=0 expect=0 ok
