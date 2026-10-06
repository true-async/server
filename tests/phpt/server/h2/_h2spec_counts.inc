<?php

/**
 * Read the final h2spec totals; a missing summary counts as a tool error.
 *
 * @return array{int, int, int} Passed cases, failed cases and tool errors.
 */
function h2spec_counts(string $report): array
{
    $errors = preg_match_all('/^Error:/m', $report);

    if (!preg_match('/^(\d+) tests, (\d+) passed, (\d+) skipped, (\d+) failed\s*$/m', $report, $totals)) {
        return [0, 0, max(1, $errors)];
    }

    return [(int)$totals[2], (int)$totals[4], $errors];
}
