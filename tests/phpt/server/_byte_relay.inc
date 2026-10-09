<?php
/* A TCP relay that counts the bytes it forwards, the oracle for the server's
 * byte counters: what the relay read from the server is what the server's
 * kernel accepted and the peer received, and what it wrote to the server is
 * what the server's socket was offered. Each accepted client gets its own
 * upstream connection to 127.0.0.1:$target and two pumps; a pump that reads EOF
 * half-closes the other side, so a close on either end reaches the other.
 *
 * $relay->up    bytes forwarded client → server
 * $relay->down  bytes forwarded server → client
 * $relay->done  pumps that have finished; 2 per connection
 *
 * Runs inside the test's coroutines; stop() closes the listening socket. */

use function Async\spawn;
use function Async\delay;

final class TasByteRelay
{
    public int $port;
    public int $up = 0;
    public int $down = 0;
    public int $done = 0;
    public int $connections = 0;

    /** @var resource */
    private $listener;

    public function __construct(private readonly int $target)
    {
        $this->listener = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);

        if ($this->listener === false) {
            throw new RuntimeException("relay listen failed: $errstr");
        }

        $name = stream_socket_get_name($this->listener, false);
        $this->port = (int) substr($name, strrpos($name, ':') + 1);

        spawn(function () {
            while (($client = @stream_socket_accept($this->listener, 30)) !== false) {
                $upstream = stream_socket_client("tcp://127.0.0.1:{$this->target}", $errno, $errstr, 5);

                if ($upstream === false) {
                    fclose($client);
                    continue;
                }

                $this->connections++;
                spawn(fn() => $this->pump($client, $upstream, 'up'));
                spawn(fn() => $this->pump($upstream, $client, 'down'));
            }
        });
    }

    private function pump($from, $to, string $direction): void
    {
        while (true) {
            $data = @fread($from, 65536);

            if ($data === false || $data === '') {
                break;
            }

            $written = @fwrite($to, $data);

            if ($written === false) {
                break;
            }

            $this->$direction += $written;

            if ($written < strlen($data)) {
                break;
            }
        }

        @stream_socket_shutdown($to, STREAM_SHUT_WR);
        $this->done++;
    }

    /* Waits until $connections connections have both pumps finished, at most
     * $timeoutMs. Answers whether they did. */
    public function waitClosed(int $connections = 1, int $timeoutMs = 5000): bool
    {
        for ($waited = 0; $waited < $timeoutMs; $waited += 10) {
            if ($this->done >= 2 * $connections) {
                return true;
            }

            delay(10);
        }

        return false;
    }

    public function stop(): void
    {
        @fclose($this->listener);
    }
}

/* The datagram counterpart: one client address, one upstream socket to
 * 127.0.0.1:$target. $up and $down count datagram payload bytes forwarded in
 * each direction, $downDatagrams the datagrams the server sent. */
final class TasUdpRelay
{
    public int $port;
    public int $up = 0;
    public int $down = 0;
    public int $downDatagrams = 0;

    /** @var resource */
    private $front;
    /** @var resource */
    private $back;
    private ?string $client = null;
    private bool $stopped = false;

    public function __construct(int $target)
    {
        $this->front = stream_socket_server('udp://127.0.0.1:0', $errno, $errstr, STREAM_SERVER_BIND);
        $this->back = stream_socket_client("udp://127.0.0.1:$target", $errno, $errstr);

        if ($this->front === false || $this->back === false) {
            throw new RuntimeException("udp relay failed: $errstr");
        }

        $name = stream_socket_get_name($this->front, false);
        $this->port = (int) substr($name, strrpos($name, ':') + 1);

        spawn(function () {
            while (!$this->stopped) {
                $data = @stream_socket_recvfrom($this->front, 65536, 0, $peer);

                if ($data === false || $data === '') {
                    break;
                }

                $this->client = $peer;
                $this->up += strlen($data);
                @fwrite($this->back, $data);
            }
        });

        spawn(function () {
            while (!$this->stopped) {
                $data = @fread($this->back, 65536);

                if ($data === false || $data === '') {
                    break;
                }

                $this->down += strlen($data);
                $this->downDatagrams++;

                if ($this->client !== null) {
                    @stream_socket_sendto($this->front, $data, 0, $this->client);
                }
            }
        });
    }

    public function stop(): void
    {
        $this->stopped = true;
        @fclose($this->front);
        @fclose($this->back);
    }
}
