<?php
/* Free port for phpt servers, claimed for the life of the test process.
 *
 * Every test needs a port no other test uses: the server binds with
 * SO_REUSEPORT, so under run-tests -j two tests on one port silently split
 * each other's traffic (h2/012 flaked exactly like that: curl got a foreign
 * server's response, rc=0, own telemetry all zero).
 *
 * A port the kernel assigns for :0 and the probe then closes is free for
 * anyone until the server binds it, and outgoing connections take their
 * local port from that same ephemeral range: core/027 lost its bind on macOS
 * to one. Windows assigns ephemeral ports in order, so a span's `base + 1` is
 * the next port a parallel test's connect() receives (core/023).
 *
 * So the ports come from TAS_PORT_LO..TAS_PORT_HI instead: below the
 * ephemeral range of Linux (32768) and of macOS and Windows (49152), where no
 * outgoing connection lands, and clear of the literal ports older tests still
 * use (18xxx to 20455, 28700 to 29699). Tests are kept apart by a lock file
 * per port, held with flock() until the process exits; a port another program
 * holds fails the probe bind and is skipped. The probe binds UDP as well,
 * because the HTTP/3 tests put their QUIC listener on the same number. */

const TAS_PORT_LO = 21000;
const TAS_PORT_HI = 27999;

/* Lock handles of the ports this process claimed, by port. Closing one
 * releases the claim; the rest are released when the process exits. */
function &tas_port_locks(): array
{
    static $locks = [];
    return $locks;
}

/* Claim $port for this process: true when no other test holds its lock and
 * nothing listens on it over TCP or UDP. */
function tas_port_claim(int $port): bool
{
    $dir = sys_get_temp_dir() . '/tas-ports';
    if (!is_dir($dir)) {
        @mkdir($dir, 0777, true);
    }

    $lock = @fopen("$dir/$port.lock", 'c');
    if ($lock === false) {
        return false;
    }

    if (!flock($lock, LOCK_EX | LOCK_NB)) {
        fclose($lock);
        return false;
    }

    $tcp = @stream_socket_server("tcp://127.0.0.1:$port", $errno, $errstr);
    $udp = @stream_socket_server("udp://127.0.0.1:$port", $errno, $errstr, STREAM_SERVER_BIND);
    $bindable = $tcp !== false && $udp !== false;

    if ($tcp !== false) { fclose($tcp); }
    if ($udp !== false) { fclose($udp); }

    if (!$bindable) {
        fclose($lock);
        return false;
    }

    tas_port_locks()[$port] = $lock;
    return true;
}

function tas_port_release(int $port): void
{
    $locks = &tas_port_locks();

    if (isset($locks[$port])) {
        fclose($locks[$port]);
        unset($locks[$port]);
    }
}

function tas_free_port(): int
{
    return tas_free_port_span(1);
}

/* A base port P such that P..P+$count-1 are all claimed — for tests that
 * derive sibling ports as $port + N (e.g. a TCP listener next to the H3 UDP
 * one). The scan starts at a random base, so parallel tests rarely contend
 * for the same lock. */
function tas_free_port_span(int $count): int
{
    $range = TAS_PORT_HI - TAS_PORT_LO + 1;
    $start = random_int(0, $range - 1);

    for ($step = 0; $step < $range; $step++) {
        $base = TAS_PORT_LO + ($start + $step) % $range;

        if ($base + $count - 1 > TAS_PORT_HI) {
            continue;
        }

        for ($i = 0; $i < $count; $i++) {
            if (!tas_port_claim($base + $i)) {
                break;
            }
        }

        if ($i === $count) {
            return $base;
        }

        for ($j = 0; $j < $i; $j++) {
            tas_port_release($base + $j);
        }
    }

    die('skip cannot allocate a free port span');
}
