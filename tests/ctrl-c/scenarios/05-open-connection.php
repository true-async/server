<?php
# CONNECT: 18935
# MARKER: SIGNAL-WOKE
/* DevServer pattern with a live keep-alive client held open by the
 * runner at the moment SIGINT arrives — a handler coroutine sits
 * suspended in fread(). Mirrors "browser tab open, then Ctrl+C". */

use Async\Future;
use Async\FutureState;
use Async\Scope;
use function Async\spawn;

$port = 18935;

$shutdownState = new FutureState();
$shutdownFuture = (new Future($shutdownState))->ignore();

spawn(function () use ($shutdownState) {
    \Async\await_any_or_fail([
        \Async\signal(\Async\Signal::SIGINT),
        \Async\signal(\Async\Signal::SIGTERM),
    ]);
    echo "SIGNAL-WOKE\n";
    $shutdownState->complete(null);
});

$serverScope = new Scope();

$serverScope->spawn(function () use ($serverScope, $port) {
    $socket = stream_socket_server("tcp://127.0.0.1:{$port}");
    if ($socket === false) {
        throw new RuntimeException("bind failed on port {$port}");
    }

    echo "READY\n";

    while (true) {
        $client = @stream_socket_accept($socket, timeout: -1);
        if ($client === false) {
            continue;
        }

        $requestScope = Scope::inherit($serverScope);
        $requestScope->spawn(function () use ($client, $requestScope) {
            try {
                /* Content-Length: 100 never arrives — stays suspended here. */
                $raw = '';
                while ($chunk = fread($client, 8192)) {
                    $raw .= $chunk;
                }
            } finally {
                $requestScope->dispose();
                fclose($client);
            }
        });
    }
});

try {
    $serverScope->awaitCompletion($shutdownFuture);
} catch (\Async\AsyncCancellation) {
    $serverScope->cancel();
}

echo "SHUTDOWN-COMPLETE\n";
