<?php
# MARKER: ASYNC-CAUGHT
/* Reverse order: pcntl_signal() first, then Async\signal().
 * uv_signal_start() overwrites pcntl's sigaction; the libuv callback is
 * expected to forward into the Zend handler chain so both fire. */

use function Async\spawn;

if (!function_exists('pcntl_signal')) {
    echo "SKIP pcntl not available\n";
    exit(0);
}

pcntl_signal(SIGINT, function () {
    echo "PCNTL-CAUGHT\n";
});

spawn(function () {
    \Async\await(\Async\signal(\Async\Signal::SIGINT));
    echo "ASYNC-CAUGHT\n";
});

echo "READY\n";
