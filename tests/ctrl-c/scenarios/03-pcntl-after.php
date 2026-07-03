<?php
# MARKER: ASYNC-CAUGHT
/* Async\signal() subscribes first, then pcntl_signal() re-registers the
 * same signal — what Symfony Console's SignalRegistry does inside every
 * artisan command. If pcntl's sigaction clobbers libuv's handler, the
 * async waiter never wakes. */

use function Async\spawn;

if (!function_exists('pcntl_signal')) {
    echo "SKIP pcntl not available\n";
    exit(0);
}

spawn(function () {
    \Async\await(\Async\signal(\Async\Signal::SIGINT));
    echo "ASYNC-CAUGHT\n";
});

spawn(function () {
    /* Let the waiter install the uv_signal handler first. */
    \Async\delay(100);
    pcntl_signal(SIGINT, function () {
        echo "PCNTL-CAUGHT\n";
    });
    echo "READY\n";
});
