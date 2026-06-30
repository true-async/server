<?php
/**
 * Evaluate an Autobahn|Testsuite fuzzingclient report.
 *
 * Reads reports/servers/index.json and exits non-zero if any case has a
 * failing verdict. Autobahn grades each case's `behavior` (frame-level)
 * and `behaviorClose` (close-handshake) as one of:
 *   OK / NON-STRICT / INFORMATIONAL  -> acceptable
 *   FAILED / WRONG CODE / UNCLEAN / UNIMPLEMENTED -> failure
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

$ACCEPT = ['OK', 'NON-STRICT', 'INFORMATIONAL'];

$pass = $fail = 0;
$failures = [];

foreach ($index[$agent] as $case => $r) {
    $behavior      = $r['behavior']      ?? 'MISSING';
    $behaviorClose = $r['behaviorClose'] ?? 'MISSING';
    $ok = in_array($behavior, $ACCEPT, true)
       && in_array($behaviorClose, $ACCEPT, true);
    if ($ok) {
        $pass++;
    } else {
        $fail++;
        $failures[] = sprintf('  %-10s behavior=%s close=%s', $case, $behavior, $behaviorClose);
    }
}

printf("Autobahn: %d passed, %d failed (%d cases)\n", $pass, $fail, $pass + $fail);

if ($fail > 0) {
    fwrite(STDERR, "FAILED cases:\n" . implode("\n", $failures) . "\n");
    exit(1);
}

echo "All cases within OK / NON-STRICT / INFORMATIONAL.\n";
exit(0);
