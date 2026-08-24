<?php

/* Refuses a phpt run in which nothing executed.
 *
 * run-tests.php exits 0 for a suite where every test skipped, which is the same
 * verdict it gives a clean run — so a job whose build produced no extension
 * passes having checked nothing (true-async/server#271). The number that
 * separates the two is on the summary line, not in the exit code.
 *
 * Usage: php assert_executed.php <run-tests output file>
 * Exit:  0 when at least one test executed, 1 otherwise, 2 when the summary is
 *        unreadable — an unparseable log is not a pass either. */

if ($argc < 2) {
    fwrite(STDERR, "usage: assert_executed.php <run-tests-output>\n");
    exit(2);
}

$log = @file_get_contents($argv[1]);

if ($log === false) {
    fwrite(STDERR, "assert_executed: cannot read {$argv[1]}\n");
    exit(2);
}

/* "Number of tests :   491                 0" — collected, then executed. */
if (!preg_match('/^Number of tests\s*:\s*(\d+)\s+(\d+)/m', $log, $m)) {
    fwrite(STDERR, "assert_executed: no summary line in {$argv[1]}\n");
    exit(2);
}

[$all, $collected, $executed] = $m;

if ((int) $executed === 0) {
    fwrite(STDERR, "assert_executed: {$collected} tests collected, none executed —"
        . " a suite that skips everything proves nothing\n");
    exit(1);
}

fwrite(STDOUT, "assert_executed: {$executed} of {$collected} executed\n");
exit(0);
