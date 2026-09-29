<?php
/* SKIPIF for a test that needs a subscriber on every worker.
 *
 * Which worker accepts a connection is not something the server promises
 * (true-async/server#240). Where the kernel load-balances SO_REUSEPORT, the
 * rounds of ws_open_spread_subscribers reach every worker in practice. On the
 * shared listen fd — the default outside Linux and FreeBSD, and the second
 * Linux CI run through the env — they measurably do not: on 2026-09-29, 32
 * WebSocket connections to 2 workers split 2:30 to 11:21 there against 10:22
 * to 22:10 on SO_REUSEPORT, and 065 ended at 0:32 on 2 to 5 runs of 20. The
 * condition mirrors core/076's, which selects the same camp.
 *
 * Usage:
 *     <?php
 *     require __DIR__ . '/_ws_spread_skipif.inc';
 *     ws_spread_skipif();
 *     ?>
 */

function ws_spread_skipif(): void
{
    if (!((PHP_OS_FAMILY === 'Linux' || PHP_OS_FAMILY === 'BSD')
          && getenv('TRUE_ASYNC_SERVER_SHARED_LISTEN_FD') !== '1')) {
        die('skip shared listen fd: no subscriber on every worker is promised (#240)');
    }
}
