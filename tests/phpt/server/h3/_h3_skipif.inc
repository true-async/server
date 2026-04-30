<?php
/* Shared SKIPIF plumbing for HTTP/3 phpt tests.
 *
 * Why a shared helper: every H3 phpt repeats the same handful of
 * preconditions (openssl CLI, h3client harness, objdump). Doing it
 * inline ties the SKIPIF block to POSIX-only `2>/dev/null` and
 * `command -v` and turns each phpt into a Linux-only test even when
 * the actual test logic is portable. Routing through h3_skipif()
 * means a single fix here unblocks all 24 tests on Windows.
 *
 * Pass an array of required preconditions; the function dies with the
 * appropriate `skip ...` message if any are unmet, otherwise returns.
 *
 * Usage:
 *     <?php
 *     require __DIR__ . '/_h3_skipif.inc';
 *     h3_skipif(['openssl_cli' => true, 'h3client' => true]);
 *     ?>
 */

function h3_skipif(array $req = []): void {
    /* Default: the extension must be loaded. Every H3 test needs this. */
    if (!class_exists('TrueAsync\\HttpServer')) {
        die('skip http_server not loaded');
    }

    $req += [
        'sockets'     => false,
        'openssl_cli' => false,
        'h3client'    => false,
        'objdump'     => false,
    ];

    if ($req['sockets'] && !extension_loaded('sockets')) {
        die('skip ext/sockets required');
    }

    if ($req['openssl_cli'] && !h3_have_cli('openssl')) {
        die('skip openssl CLI not available');
    }

    if ($req['objdump']) {
        /* objdump ships with binutils — present by default on Linux,
         * absent on Windows unless the user installed mingw-w64 or
         * similar. The 104 test (.text section sanity-check) genuinely
         * needs the Linux ELF tooling, no point pretending otherwise. */
        if (PHP_OS_FAMILY === 'Windows') {
            die('skip objdump not available on Windows');
        }
        if (!h3_have_cli('objdump')) {
            die('skip objdump not available');
        }
    }

    if ($req['h3client']) {
        /* tests/h3client/h3client.c uses unistd.h / poll.h / netdb.h /
         * sys/socket.h directly — it's a Linux-targeted harness, not
         * portable to Winsock. Skip on Windows; the same coverage is
         * supposed to come from CI on Linux. */
        if (PHP_OS_FAMILY === 'Windows') {
            die('skip h3client harness is POSIX-only');
        }
        $client = __DIR__ . '/../../../h3client/h3client';
        if (!is_executable($client)) {
            $src = __DIR__ . '/../../../h3client/h3client.c';
            if (!is_file($src)) {
                die('skip h3client source missing');
            }
            $rc = 0; $out = [];
            exec('gcc -O2 -std=c11 -o ' . escapeshellarg($client) . ' '
               . escapeshellarg($src)
               . ' -I/usr/local/include -L/usr/local/lib '
               . '-Wl,-rpath,/usr/local/lib '
               . '-lngtcp2 -lngtcp2_crypto_ossl -lnghttp3 -lssl -lcrypto 2>&1',
                 $out, $rc);
            if ($rc !== 0) {
                die("skip h3client build failed: " . implode("\n", $out));
            }
        }
    }
}

/* Cross-platform "is this CLI tool available". `2>&1` redirect works
 * in both bash and cmd.exe — the original `2>/dev/null` blew up on
 * Windows because cmd has no `/dev/null` and treated the path as a
 * literal target file name. Output goes into the $out array, so the
 * console stays clean on either platform. */
function h3_have_cli(string $cmd): bool {
    $rc = 0; $out = [];
    @exec(escapeshellarg($cmd) . ' --version 2>&1', $out, $rc);
    return $rc === 0;
}

/* Cross-platform "throw away stderr" suffix. `/dev/null` only exists
 * on POSIX; `nul` is the cmd.exe equivalent. Used to silence openssl's
 * progress dots ("..............+++") that otherwise flood phpt
 * output and break expected-output diffs. */
function h3_dev_null(): string {
    return PHP_OS_FAMILY === 'Windows' ? 'nul' : '/dev/null';
}

/* Generate a self-signed cert+key pair for the H3 listener. QUIC
 * mandates TLS 1.3, so every E2E phpt needs a cert; doing it inline
 * with `exec('openssl req ...')` ties the test to POSIX shell quoting
 * and `/dev/null`. Returns true on success.
 *
 * Windows note: shipping OpenSSL builds default OPENSSLDIR to a
 * "C:\Program Files\Common Files\SSL" path that doesn't exist on most
 * machines, so `openssl req` fails reading openssl.cnf before it ever
 * gets to crypto. We point OPENSSL_CONF at the .cnf bundled with the
 * deps tree (E:\php\deps\openssl.cnf) when the env var isn't already
 * set, otherwise leave the user's choice alone. */
function h3_gen_cert(string $key_path, string $cert_path): bool {
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
    $devnull = h3_dev_null();
    $cmd = sprintf(
        'openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
        . '-subj "/CN=localhost" -keyout %s -out %s 2>%s',
        escapeshellarg($key_path), escapeshellarg($cert_path), $devnull);
    $rc = 0; $out = [];
    exec($cmd, $out, $rc);
    return $rc === 0 && is_file($cert_path) && is_file($key_path);
}
