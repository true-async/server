<?php

/* Refuses a phpt run in which a test outside the baseline passed on its retry.
 *
 * run-tests.php runs a failed test a second time when its FILE section calls a
 * clock function or its output says "timed out" (is_flaky, is_flaky_output), and
 * reports the second verdict only. A test that fails once in two runs therefore
 * passes the job, and its first failure survives as one line of the WARNED TEST
 * SUMMARY (true-async/server#313). The baseline lists the tests known to do this;
 * a retry of any other test fails the job, so the list can shrink and not grow.
 *
 * Usage: php assert_no_new_retries.php <baseline> <run-tests output file>...
 * Exit:  0 when every retried test is in the baseline, 1 when one is not, 2 when
 *        the baseline or a log is unreadable — a log without the summary is not a
 *        pass either. */

const RETRY_LINE = '/\[([^\]\r\n]+)\] \(warn: Test passed on retry attempt\)/';
const SUITE_ROOT = 'tests/phpt/';

if ($argc < 3) {
    fwrite(STDERR, "usage: assert_no_new_retries.php <baseline> <run-tests-output>...\n");
    exit(2);
}

$baseline = read_baseline($argv[1]);
$unexpected = [];

foreach (array_slice($argv, 2) as $log_file) {
    foreach (retried_tests($log_file) as $test) {
        if (isset($baseline[$test])) {
            fwrite(STDOUT, "assert_no_new_retries: {$test} passed on retry (in the baseline)\n");
        } else {
            $unexpected[] = $test;
        }
    }
}

if ($unexpected !== []) {
    fwrite(STDERR, "assert_no_new_retries: failed its first run and passed only on the retry:\n");
    foreach ($unexpected as $test) {
        fwrite(STDERR, "  {$test}\n");
    }
    fwrite(STDERR, "Reproduce it outside run-tests and fix it; the baseline"
        . " in {$argv[1]} is for tests that already retried when it was written.\n");
    exit(1);
}

fwrite(STDOUT, "assert_no_new_retries: no retry outside the baseline\n");
exit(0);

/* Baseline entries are paths relative to tests/phpt, one per line; '#' starts a
 * comment. Returns them as the keys of a set. */
function read_baseline(string $file): array
{
    $lines = @file($file, FILE_IGNORE_NEW_LINES);

    if ($lines === false) {
        fwrite(STDERR, "assert_no_new_retries: cannot read {$file}\n");
        exit(2);
    }

    $tests = [];

    foreach ($lines as $line) {
        $entry = trim(preg_replace('/#.*/', '', $line));

        if ($entry !== '') {
            $tests[$entry] = true;
        }
    }

    return $tests;
}

/* The tests a run-tests log lists as passed on retry, as paths relative to
 * tests/phpt with forward slashes, whatever the platform printed. */
function retried_tests(string $file): array
{
    $log = @file_get_contents($file);

    if ($log === false) {
        fwrite(STDERR, "assert_no_new_retries: cannot read {$file}\n");
        exit(2);
    }

    /* A run that died before its summary never printed the warned list either. */
    if (!preg_match('/^Number of tests\s*:/m', $log)) {
        fwrite(STDERR, "assert_no_new_retries: no summary line in {$file}\n");
        exit(2);
    }

    preg_match_all(RETRY_LINE, $log, $matches);
    $tests = [];

    foreach ($matches[1] as $path) {
        $path = str_replace('\\', '/', $path);
        $root = strpos($path, SUITE_ROOT);

        if ($root === false) {
            fwrite(STDERR, "assert_no_new_retries: {$path} is not under " . SUITE_ROOT . "\n");
            exit(2);
        }

        $tests[] = substr($path, $root + strlen(SUITE_ROOT));
    }

    return $tests;
}
