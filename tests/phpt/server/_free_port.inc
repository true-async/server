<?php
/* Kernel-allocated free TCP port for phpt servers.
 *
 * The legacy `BASE + getmypid() % N` pattern hands out overlapping ranges
 * (several tests even share a literal base), and the server binds with
 * SO_REUSEPORT — so under run-tests -j two tests can silently share one
 * port and split each other's traffic (h2/012 flaked exactly like that:
 * curl got a foreign server's response, rc=0, own telemetry all zero).
 *
 * Binding :0 lets the kernel pick from the ephemeral range, which the
 * legacy 19xxx bases never touch, and concurrent :0 allocations are kept
 * apart by the kernel itself. The close-to-bind window is theoretically
 * racy but the ephemeral allocator cycles, so immediate reuse by another
 * probe is practically impossible. */
function tas_free_port(): int
{
    $srv = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
    if ($srv === false) {
        die("skip cannot probe for a free port: $errstr");
    }

    $name = stream_socket_get_name($srv, false);
    fclose($srv);

    return (int) substr($name, strrpos($name, ':') + 1);
}

/* A base port P such that P..P+$count-1 are all bindable — for tests that
 * derive sibling ports as $port + N (e.g. a TCP listener next to the H3 UDP
 * one). Retries with fresh kernel-assigned bases until a whole span fits. */
function tas_free_port_span(int $count): int
{
    for ($try = 0; $try < 32; $try++) {
        $base = tas_free_port();
        $held = [];
        $ok = true;

        for ($i = 0; $i < $count; $i++) {
            $h = @stream_socket_server("tcp://127.0.0.1:" . ($base + $i), $e, $m);
            if ($h === false) { $ok = false; break; }
            $held[] = $h;
        }

        foreach ($held as $h) {
            fclose($h);
        }

        if ($ok) {
            return $base;
        }
    }

    die('skip cannot allocate a free port span');
}
