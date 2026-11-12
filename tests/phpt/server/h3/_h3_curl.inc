<?php
/* curl as an HTTP/3 client for tests that cannot use the POSIX-only h3client
 * or aioquic harnesses. */

/* The curl first on PATH. On Windows proc_open() with an argument array calls
 * CreateProcess, which searches System32 before PATH and would start the
 * System32 curl, built without HTTP/3; `where` follows PATH as cmd does. */
function h3_curl_bin(): string {
    static $bin = null;
    if ($bin === null) {
        $bin = 'curl';
        if (PHP_OS_FAMILY === 'Windows') {
            $first = strtok((string) shell_exec('where curl 2>nul'), "\r\n");
            if ($first !== false && $first !== '') {
                $bin = $first;
            }
        }
    }

    return $bin;
}

/* Runs curl with $args, no shell between, and returns [exit code, stdout];
 * stderr is discarded. */
function h3_curl(array $args): array {
    $null = PHP_OS_FAMILY === 'Windows' ? 'nul' : '/dev/null';
    $proc = proc_open(array_merge([h3_curl_bin()], $args),
        [0 => ['file', $null, 'r'], 1 => ['pipe', 'w'], 2 => ['file', $null, 'w']], $pipes);
    if (!is_resource($proc)) {
        return [-1, ''];
    }

    $out = (string) stream_get_contents($pipes[1]);
    fclose($pipes[1]);

    return [proc_close($proc), $out];
}
