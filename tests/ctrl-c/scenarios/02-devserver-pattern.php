<?php
# MARKER: SIGNAL-WOKE
/* Faithful replica of laravel-spawn DevServer::start()
 * (YanGusik/laravel-spawn#8): signal waiter completing a FutureState
 * used as the cancellation for Scope::awaitCompletion() over an
 * accept-loop scope. */

use Async\Future;
use Async\FutureState;
use Async\Scope;
use function Async\spawn;

$port = (int) (getenv('CTRLC_PORT') ?: 18931);

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
                $raw = '';
                while ($chunk = fread($client, 8192)) {
                    $raw .= $chunk;
                    if (str_contains($raw, "\r\n\r\n")) {
                        break;
                    }
                }
                fwrite($client, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
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
