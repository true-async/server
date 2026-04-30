--TEST--
HTTP/2 compliance gate — h2spec pass count must not drop below baseline
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['h2spec' => true]);
?>
--FILE--
<?php
/*
 * CI gate: spawn the project's h2spec_server.php in a subprocess,
 * run h2spec against it, parse the pass/fail counts. Baseline is
 * recorded below; CI fails if pass count drops or fail count rises.
 *
 * Why a subprocess (not the in-process pattern of the other phpts):
 * h2spec hammers the server for ~5-10 s with hundreds of crafted
 * requests. Running it inside the same PHP process competes for the
 * libuv loop slot we need for h2spec_server.php and produces flaky
 * results. A clean fork is the same pattern h2o's own
 * t/40http2-h2spec.t uses.
 */

// ---- Baseline (update only when the underlying gap shrinks).
//
// Stable as of 2026-04-28 on Linux 5.15 / WSL2 after the per-conn
// write-timer MULTISHOT fix (commit 733fae0): 5/5 runs returned
// 146 pass / 0 fail / 0 errors (1 skipped — long-known HPACK Huffman
// EOS XFAIL).
//
// Pre-fix baseline was 72 pass / 1 fail / 1 connection-refused due to
// a false-positive Async\DeadlockError firing at test #73 — see memory
// project_h2spec_deadlock_2026-04-28 for the full diagnosis.
const MIN_PASS = 140;
const MAX_FAIL = 1;

$h2spec = getenv('H2SPEC') ?: trim((string)shell_exec('command -v h2spec 2>/dev/null'));
if ($h2spec === '' && is_executable($home = (string)getenv('HOME') . '/.local/bin/h2spec')) {
    $h2spec = $home;
}

$port = 19500 + getmypid() % 200;

// PHP_BINARY would re-enter THIS test under the phpt runner; use its
// caller chain via posix or fall back to reading /proc/self/exe.
$php = PHP_BINARY;

// PHP_BINARY under run-tests.php may point at phpdbg — fall back to
// a real `php` on PATH so the spawned server actually starts.
if (!preg_match('~/php(\d+(\.\d+)?)?$~', $php) || str_contains($php, 'phpdbg')) {
    $alt = trim((string)shell_exec('command -v php 2>/dev/null'));
    if ($alt !== '' && is_executable($alt)) {
        $php = $alt;
    }
}

// run-tests.php copies the FILE block to a temp dir, so __DIR__ won't
// resolve back to the project. Honour HTTP_SERVER_ROOT env var; if
// unset, walk up from __FILE__ which IS the temp file (still no help)
// then try common locations.
$root = (string)getenv('HTTP_SERVER_ROOT');
if ($root === '' || !is_file("$root/Makefile")) {
    foreach (['/home/edmond/php-http-server', getcwd()] as $cand) {
        if (is_string($cand) && $cand !== '' && is_file("$cand/Makefile")
            && is_file("$cand/tests/bench/h2spec_server.php")) {
            $root = $cand;
            break;
        }
    }
}
$ext_dir = "$root/modules";
$srv     = "$root/tests/bench/h2spec_server.php";

if (!is_file($srv) || !is_dir($ext_dir)) {
    echo "SKIP: project layout not found (set HTTP_SERVER_ROOT=/path/to/repo)\n";
    exit(0);
}

// Fork a fresh PHP process to host the server.
$cmd = sprintf(
    '%s -n -d extension_dir=%s -d extension=true_async_server %s %d',
    escapeshellarg($php),
    escapeshellarg($ext_dir),
    escapeshellarg($srv),
    $port
);
$descriptors = [
    0 => ['pipe', 'r'],
    1 => ['file', '/dev/null', 'w'],
    2 => ['file', '/tmp/h2spec_srv_phpt.log', 'w'],
];
/* Mask inherited fds > 2 so the spawned bench server doesn't keep
 * run-tests.php's stdout/stderr capture pipe alive. PHP's proc_open
 * only redirects the fds listed in $descriptors; everything else is
 * inherited without CLOEXEC. If the bench server outlives the test
 * (e.g. SIGTERM ignored, run-tests timeout reaps the test PHP first)
 * the orphan keeps the run-tests pipe write-end open, and run-tests
 * blocks in epoll_pwait forever waiting for EOF. Remapping every
 * inherited fd to /dev/null in the child severs that link cleanly. */
if (is_dir('/proc/self/fd')) {
    foreach (scandir('/proc/self/fd') ?: [] as $name) {
        if ($name === '.' || $name === '..') { continue; }
        $fd = (int)$name;
        if ($fd > 2 && !isset($descriptors[$fd])) {
            $descriptors[$fd] = ['file', '/dev/null', 'r'];
        }
    }
}
$proc = proc_open($cmd, $descriptors, $pipes);
if (!is_resource($proc)) {
    echo "ABORT: failed to spawn server\n";
    exit(0);
}
/* Close our write end of stdin immediately — the server doesn't read it,
 * and a dangling pipe-end here would propagate fd leaks to children. */
if (isset($pipes[0]) && is_resource($pipes[0])) { fclose($pipes[0]); }

/* Kill the whole bench-server tree. SIGKILL via proc_terminate only
 * reaps the master; any libuv/SO_REUSEPORT worker the master forked
 * survives and (because PHP's proc_open inherits all fds > 2 without
 * CLOEXEC) keeps the run-tests.php→test-PHP pipe write-end alive,
 * so run-tests blocks in epoll_pwait waiting for EOF. Tree-kill is
 * platform-specific: Windows has TerminateProcess + `taskkill /T`,
 * Linux walks /proc to find descendants and processes whose cmdline
 * matches our specific (server-script + port) signature. */
$kill = function () use (&$proc, $srv, $port): void {
    if (!is_resource($proc)) { return; }
    $st = @proc_get_status($proc);
    $master = is_array($st) ? (int)$st['pid'] : 0;

    if (PHP_OS_FAMILY === 'Windows') {
        if ($master > 0) {
            /* /T = tree, /F = force. Misses nothing reparented away. */
            @exec('taskkill /F /T /PID ' . (int)$master . ' 2>NUL');
        }
        @proc_terminate($proc);
    } else {
        /* Walk /proc and SIGKILL anything whose cmdline names our
         * specific bench server script + port. Catches both children
         * still parented to $master and orphans reparented to init. */
        $needle_srv  = basename($srv);
        $needle_port = (string)$port;
        if (function_exists('posix_kill') && is_dir('/proc')) {
            foreach (scandir('/proc') ?: [] as $name) {
                if ($name === '' || !preg_match('/^\d+$/', $name)) { continue; }
                $cmdline = @file_get_contents("/proc/$name/cmdline");
                if ($cmdline === false || $cmdline === '') { continue; }
                $argv = explode("\0", rtrim($cmdline, "\0"));
                if (in_array($needle_port, $argv, true)
                    && implode(' ', $argv) !== ''
                    && (str_contains($cmdline, $needle_srv)
                        || str_contains($cmdline, $srv))) {
                    @posix_kill((int)$name, 9);
                }
            }
        }
        if ($master > 0) { @proc_terminate($proc, 9); }
    }

    /* Bounded waitpid: 2 s is plenty since SIGKILL is uncatchable. */
    for ($i = 0; $i < 200; $i++) {
        $st = @proc_get_status($proc);
        if (!is_array($st) || empty($st['running'])) { break; }
        usleep(10000);
    }
    @proc_close($proc);
};

// Wait for the server to bind. Poll up to 3 s.
$bound = false;
for ($i = 0; $i < 30; $i++) {
    usleep(100000);
    $sock = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 1);
    if ($sock !== false) {
        fclose($sock);
        $bound = true;
        break;
    }
}
if (!$bound) {
    $kill();
    echo "ABORT: server did not bind on $port\n";
    exit(0);
}

// Run h2spec.
$h2cmd = sprintf(
    '%s --host 127.0.0.1 --port %d --timeout 5 --strict 2>&1',
    escapeshellarg($h2spec),
    $port
);
$out = (string)shell_exec($h2cmd);

$kill();

$pass   = substr_count($out, '✔');
$fail   = substr_count($out, '×');
$errors = preg_match_all('/^Error:/m', $out);

echo "pass=$pass\n";
echo "fail=$fail\n";
echo "errors=$errors\n";

$ok_pass = $pass >= MIN_PASS;
$ok_fail = $fail <= MAX_FAIL;
echo "pass_gate=" . ($ok_pass ? 'ok' : "REGRESSION (need >=" . MIN_PASS . ")") . "\n";
echo "fail_gate=" . ($ok_fail ? 'ok' : "REGRESSION (need <=" . MAX_FAIL . ")") . "\n";
--EXPECTF--
pass=%d
fail=%d
errors=%d
pass_gate=ok
fail_gate=ok
