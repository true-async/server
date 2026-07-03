<?php
# MARKER: ALL-CAUGHT
/* Three independent coroutines waiting on the same SIGINT — mirrors the
 * three identical suspended waiters in the macOS zombie log. All three
 * must wake from a single delivery. */

use function Async\spawn;

$caught = 0;

for ($i = 0; $i < 3; $i++) {
    spawn(function () use (&$caught) {
        \Async\await(\Async\signal(\Async\Signal::SIGINT));
        $caught++;
        echo "CAUGHT-{$caught}\n";
        if ($caught === 3) {
            echo "ALL-CAUGHT\n";
        }
    });
}

echo "READY\n";
