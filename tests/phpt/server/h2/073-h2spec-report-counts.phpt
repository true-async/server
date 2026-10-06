--TEST--
h2spec report totals count failed cases once and reject incomplete reports
--FILE--
<?php
require __DIR__ . '/_h2spec_counts.inc';

$report = "  ✔ 1: valid case\n  × 2: invalid case\nFailures:\n  × 2: invalid case\n"
    . "Finished in 1 seconds\n3 tests, 1 passed, 1 skipped, 1 failed\n";
echo json_encode(h2spec_counts($report)), "\n";
echo json_encode(h2spec_counts("147 tests, 146 passed, 1 skipped, 0 failed\n")), "\n";
echo json_encode(h2spec_counts("Error: connection refused\n")), "\n";
echo json_encode(h2spec_counts("✔ 1: incomplete report\n")), "\n";
--EXPECT--
[1,1,0]
[146,0,0]
[0,0,1]
[0,0,1]
