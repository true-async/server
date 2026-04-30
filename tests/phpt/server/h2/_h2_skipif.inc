<?php
/* Shared SKIPIF plumbing for HTTP/2 phpt tests.
 *
 * Mirror of tests/phpt/server/h3/_h3_skipif.inc — the H2 suite has the
 * same Linux-only pattern (`exec('curl --version 2>/dev/null')`,
 * `command -v h2spec`, raw `openssl req` cert generation) and breaks
 * SKIPIF parsing on Windows for the same reason. Centralising the
 * plumbing lets a single fix here unblock all 19 H2 tests.
 *
 * Usage:
 *     <?php
 *     require __DIR__ . '/_h2_skipif.inc';
 *     h2_skipif(['curl_h2' => true, 'openssl_cli' => true]);
 *     ?>
 */

function h2_skipif(array $req = []): void {
    $req += [
        'curl'        => false,   /* curl CLI (any) — h2 tests don't need this directly, h2_curl pulls it in */
        'curl_h2'     => false,   /* curl CLI built with nghttp2 — Windows curl is Schannel, no nghttp2 */
        'openssl_cli' => false,
        'h2spec'      => false,
        'h2load'      => false,
    ];

    if ($req['curl'] && !h2_have_cli('curl')) {
        die('skip curl CLI not available');
    }

    if ($req['curl_h2']) {
        if (!h2_have_cli('curl')) {
            die('skip curl CLI not available');
        }
        $out = []; $rc = 0;
        @exec('curl --version 2>&1', $out, $rc);
        if ($rc !== 0 || !preg_grep('/nghttp2/', $out)) {
            die('skip curl lacks HTTP/2 support');
        }
    }

    if ($req['openssl_cli'] && !h2_have_cli('openssl')) {
        die('skip openssl CLI not available');
    }

    if ($req['h2spec']) {
        /* h2spec is a Go binary the project pins for RFC 7540 conformance.
         * No prebuilt Windows distribution is shipped with the repo, and
         * the existing `command -v` probe is POSIX-only. Skip on Windows
         * unless the user explicitly points H2SPEC at a binary. */
        $h2spec = getenv('H2SPEC') ?: '';
        if ($h2spec === '' && PHP_OS_FAMILY !== 'Windows') {
            $h2spec = trim((string)shell_exec('command -v h2spec 2>/dev/null'));
            if ($h2spec === '') {
                $home = (string)getenv('HOME') . '/.local/bin/h2spec';
                if (is_executable($home)) {
                    $h2spec = $home;
                }
            }
        }
        if ($h2spec === '' || !is_executable($h2spec)) {
            die('skip h2spec binary not found (set H2SPEC=/path/to/h2spec)');
        }
    }

    if ($req['h2load'] && !h2_have_cli('h2load')) {
        die('skip h2load not available');
    }
}

/* Cross-platform "is this CLI available". `2>&1` works in both bash
 * and cmd.exe; output goes into the array, console stays clean. */
function h2_have_cli(string $cmd): bool {
    $rc = 0; $out = [];
    @exec(escapeshellarg($cmd) . ' --version 2>&1', $out, $rc);
    return $rc === 0;
}

/* Cross-platform discard target. /dev/null only exists on POSIX;
 * `nul` is the cmd.exe equivalent. Used in -o/2> redirects. */
function h2_dev_null(): string {
    return PHP_OS_FAMILY === 'Windows' ? 'nul' : '/dev/null';
}

/* Self-signed cert+key for tests that need TLS. Mirrors h3_gen_cert.
 * The prebuilt Windows OpenSSL bundles point OPENSSLDIR at a
 * "C:\Program Files\Common Files\SSL" path that's typically absent;
 * pointing OPENSSL_CONF at the bundled openssl.cnf side-steps the
 * "Can't open .../openssl.cnf for reading" failure before crypto. */
function h2_gen_cert(string $key_path, string $cert_path): bool {
    if (PHP_OS_FAMILY === 'Windows' && getenv('OPENSSL_CONF') === false) {
        $candidates = [
            'E:\\php\\deps\\openssl.cnf',
            getenv('PHP_PHP_BUILD') ? getenv('PHP_PHP_BUILD') . '\\openssl.cnf' : null,
        ];
        foreach ($candidates as $c) {
            if ($c !== null && is_file($c)) {
                putenv('OPENSSL_CONF=' . $c);
                break;
            }
        }
    }
    $devnull = h2_dev_null();
    $cmd = sprintf(
        'openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
        . '-subj "/CN=localhost" -keyout %s -out %s 2>%s',
        escapeshellarg($key_path), escapeshellarg($cert_path), $devnull);
    $rc = 0; $out = [];
    exec($cmd, $out, $rc);
    return $rc === 0 && is_file($cert_path) && is_file($key_path);
}
