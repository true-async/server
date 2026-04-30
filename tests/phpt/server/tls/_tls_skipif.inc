<?php
/* Shared SKIPIF plumbing for TLS phpt tests.
 *
 * Same shape as _h3_skipif.inc / _h2_skipif.inc — TLS tests inline
 * `exec('openssl version 2>/dev/null')` which trips Windows cmd.exe
 * because /dev/null is a literal path there. h3_gen_cert / h2_gen_cert
 * pattern is repeated here too: prebuilt MSVC OpenSSL needs
 * OPENSSL_CONF pointing at a real .cnf or `openssl req` errors out
 * before any RSA work happens.
 *
 * Usage:
 *     <?php
 *     require __DIR__ . '/_tls_skipif.inc';
 *     tls_skipif(['openssl_cli' => true, 'curl' => true]);
 *     ?>
 */

function tls_skipif(array $req = []): void {
    $req += [
        'openssl_cli' => false,
        'php_ssl'     => false,   /* tests that need stream_socket_client("ssl://") */
        'curl'        => false,
        'curl_h2'     => false,
        'proc_open'   => false,
        'linux_only'  => false,   /* kTLS, AF_NETLINK, etc. */
    ];

    if ($req['linux_only'] && PHP_OS_FAMILY !== 'Linux') {
        die('skip Linux-only test (kTLS / netlink / etc.)');
    }
    if ($req['proc_open'] && !function_exists('proc_open')) {
        die('skip proc_open disabled');
    }
    if ($req['openssl_cli'] && !tls_have_cli('openssl')) {
        die('skip openssl CLI not available');
    }
    if ($req['php_ssl'] && !in_array('ssl', stream_get_transports(), true)) {
        /* PHP CLI built without ext/openssl: no ssl:// transport, so any
         * helper using stream_socket_client("ssl://...") cannot connect. */
        die('skip PHP ssl:// transport not available (ext/openssl missing in CLI build)');
    }
    if ($req['curl'] && !tls_have_cli('curl')) {
        die('skip curl CLI not available');
    }
    if ($req['curl_h2']) {
        if (!tls_have_cli('curl')) {
            die('skip curl CLI not available');
        }
        $out = []; $rc = 0;
        @exec('curl --version 2>&1', $out, $rc);
        if ($rc !== 0 || !preg_grep('/nghttp2/', $out)) {
            die('skip curl lacks HTTP/2 support');
        }
    }
}

function tls_have_cli(string $cmd): bool {
    $rc = 0; $out = [];
    @exec(escapeshellarg($cmd) . ' --version 2>&1', $out, $rc);
    return $rc === 0;
}

function tls_dev_null(): string {
    return PHP_OS_FAMILY === 'Windows' ? 'nul' : '/dev/null';
}

function tls_gen_cert(string $key_path, string $cert_path,
                     int $rsa_bits = 2048, string $hash = 'sha256'): bool {
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
    $devnull = tls_dev_null();
    $cmd = sprintf(
        'openssl req -x509 -newkey rsa:%d -%s -days 1 -nodes '
        . '-subj "/CN=localhost" -keyout %s -out %s 2>%s',
        $rsa_bits, $hash,
        escapeshellarg($key_path), escapeshellarg($cert_path), $devnull);
    $rc = 0; $out = [];
    exec($cmd, $out, $rc);
    return $rc === 0 && is_file($cert_path) && is_file($key_path);
}
