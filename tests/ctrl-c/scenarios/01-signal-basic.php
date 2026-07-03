<?php
# MARKER: ASYNC-CAUGHT
/* Minimal case: one coroutine awaiting Async\signal(SIGINT).
 * One SIGINT must wake it and let the process exit. */

use function Async\spawn;

spawn(function () {
    \Async\await(\Async\signal(\Async\Signal::SIGINT));
    echo "ASYNC-CAUGHT\n";
});

echo "READY\n";
