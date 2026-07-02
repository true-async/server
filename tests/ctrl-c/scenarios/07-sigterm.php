<?php
# SIGNAL: TERM
# MARKER: ASYNC-CAUGHT
/* Same as 01 but for SIGTERM — checks whether the effect is
 * SIGINT-specific (terminal/tty path) or generic signal delivery. */

use function Async\spawn;

spawn(function () {
    \Async\await(\Async\signal(\Async\Signal::SIGTERM));
    echo "ASYNC-CAUGHT\n";
});

echo "READY\n";
