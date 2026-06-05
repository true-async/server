--TEST--
StaticHandler: POSIX-only root-path validation (filesystem-root semantics)
--EXTENSIONS--
true_async_server
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') {
    die("skip POSIX-only: '/'-rooted filesystem semantics; on Windows "
        . "IS_ABSOLUTE_PATH requires a drive letter, so these inputs fail "
        . "earlier with \"must be an absolute path\" — covered by 016 with "
        . "OS-appropriate inputs");
}
?>
--FILE--
<?php
/* POSIX-only companion to 016-static-handler-validation. These assert
 * root semantics that only exist on *nix:
 *   - a bare "/" is absolute, exists and is a directory, so it reaches
 *     the explicit "must not be '/'" guard (last check, on the resolved
 *     path);
 *   - a leading-slash path is absolute, so a missing one passes the
 *     absolute-path gate and fails at the existence check.
 * On Windows both inputs are non-absolute (no drive letter) and fail at
 * the absolute-path gate instead — see 016 for the cross-platform arms. */

use TrueAsync\StaticHandler;

function check(string $label, callable $fn): void
{
    try {
        $fn();
        echo "$label: OK\n";
    } catch (\Throwable $e) {
        echo "$label: ", $e::class, ": ", $e->getMessage(), "\n";
    }
}

/* "/" resolves to the filesystem root → explicit guard. */
check('ctor:root-slash',       fn() => new StaticHandler('/x/', '/'));

/* Leading slash is absolute on POSIX → a missing one reaches not-found
 * (proving it cleared the absolute-path gate, unlike on Windows). */
check('ctor:root-missing-abs', fn() => new StaticHandler('/x/', '/nonexistent-' . bin2hex(random_bytes(8))));

echo "done\n";
?>
--EXPECTF--
ctor:root-slash: TrueAsync\HttpServerInvalidArgumentException: StaticHandler root directory must not be '/'
ctor:root-missing-abs: TrueAsync\HttpServerInvalidArgumentException: StaticHandler root directory not found: %s
done
