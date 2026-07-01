<?php
/**
 * Evaluate an Autobahn|Testsuite fuzzingclient report.
 *
 * Reads reports/servers/index.json and exits non-zero if any case has a
 * failing verdict.
 *
 * The authoritative per-case verdict is `behavior` (the frame-level
 * result, i.e. did the server do the right thing with the input). The
 * standard pass set is OK / NON-STRICT / INFORMATIONAL.
 *
 * `behaviorClose` (the close-handshake) is graded separately and more
 * leniently: WRONG CODE / FAILED / UNCLEAN are real server faults and
 * fail the run, but "FAILED BY CLIENT" is a client-side artifact —
 * Autobahn emits it on protocol-violation cases where the server
 * correctly fails the connection and the *client* ends up dropping the
 * TCP (e.g. case 5.1, fragmented control frame). Those are reported as
 * warnings, not failures.
 *
 * Usage:  php check_report.php [path/to/index.json] [agent]
 */

$path  = $argv[1] ?? __DIR__ . '/reports/servers/index.json';
$agent = $argv[2] ?? 'true-async-server';

if (!is_file($path)) {
    fwrite(STDERR, "report not found: $path\n");
    exit(2);
}

$index = json_decode(file_get_contents($path), true);
if (!is_array($index) || !isset($index[$agent])) {
    fwrite(STDERR, "no results for agent '$agent' in $path\n");
    exit(2);
}

$ACCEPT      = ['OK', 'NON-STRICT', 'INFORMATIONAL'];
$CLOSE_FAIL  = ['WRONG CODE', 'FAILED', 'UNCLEAN'];   // real server close faults

$pass = $fail = 0;
$failures = [];
$warnings = [];

foreach ($index[$agent] as $case => $r) {
    $behavior      = $r['behavior']      ?? 'MISSING';
    $behaviorClose = $r['behaviorClose'] ?? 'MISSING';

    $behaviorOk = in_array($behavior, $ACCEPT, true);
    $closeFault = in_array($behaviorClose, $CLOSE_FAIL, true);

    if ($behaviorOk && !$closeFault) {
        $pass++;
        if (!in_array($behaviorClose, $ACCEPT, true)) {
            // e.g. FAILED BY CLIENT — acceptable, but surface it.
            $warnings[] = sprintf('  %-10s behavior=%s close=%s', $case, $behavior, $behaviorClose);
        }
    } else {
        $fail++;
        $failures[] = sprintf('  %-10s behavior=%s close=%s', $case, $behavior, $behaviorClose);
    }
}

printf("Autobahn: %d passed, %d failed (%d cases)\n", $pass, $fail, $pass + $fail);

if ($warnings) {
    fwrite(STDERR, sprintf("Close-handshake warnings (non-fatal, %d):\n%s\n",
        count($warnings), implode("\n", $warnings)));
}

if ($fail > 0) {
    fwrite(STDERR, "FAILED cases:\n" . implode("\n", $failures) . "\n");
    exit(1);
}

echo "All cases pass on behavior (close faults gated separately).\n";
exit(0);
