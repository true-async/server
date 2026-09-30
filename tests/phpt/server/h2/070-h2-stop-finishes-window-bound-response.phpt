--TEST--
HTTP/2 h2c: stop() sends GOAWAY and lets a response held by the flow-control window finish (#345)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* stop() takes every connection out of service. An HTTP/2 connection gets a
 * GOAWAY and closes once its streams are done; a handler that has returned is
 * not one of them, but its body may still wait on the peer's window. Here the
 * handler stops the server and answers 256 KiB against the default 64 KiB
 * stream window. The client reads the first window, holds the refill until
 * start() has returned, and then must still receive the whole body. */

require_once __DIR__ . '/_h2_client.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = tas_free_port();
$size = 256 * 1024;
$body = str_repeat('B', $size);

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setShutdownTimeout(1));

$server->addHttpHandler(function ($req, $res) use ($server, $body) {
    $server->stop();
    $res->setStatusCode(200)->setBody($body);
});

$returned = false;
$client = spawn(function () use ($port, $size, $body, &$returned) {
    try {
        $c = new H2TestClient('127.0.0.1', $port, 8);
        $sid = $c->sendRequest('GET', '/big', "127.0.0.1:$port");

        $got = '';
        $goaway = false;
        $ended = false;
        $refilled = false;

        while (($fr = $c->readFrame()) !== null) {
            [$type, $flags, $fsid, $payload] = $fr;

            if ($type === H2_FRAME_SETTINGS && !($flags & H2_FLAG_ACK)) {
                $c->sendSettingsAck();
                continue;
            }

            if ($type === H2_FRAME_GOAWAY) {
                $goaway = true;
                continue;
            }

            if ($type !== H2_FRAME_DATA || $fsid !== $sid) {
                continue;
            }

            $got .= $payload;

            if ($flags & H2_FLAG_END_STREAM) {
                $ended = true;
                break;
            }

            if (!$refilled && strlen($got) >= 65535) {
                for ($i = 0; $i < 50 && !$returned; $i++) {
                    delay(20);
                }

                echo "window refilled after start() returned: ", $returned ? 'yes' : 'no', "\n";
                $c->sendWindowUpdate(0, $size);
                $c->sendWindowUpdate($sid, $size);
                $refilled = true;
            }
        }

        echo "goaway: ", $goaway ? 'yes' : 'no', "\n";
        echo "end_stream: ", $ended ? 'yes' : 'no', "\n";
        echo "bytes: ", strlen($got), " of $size, ", $got === $body ? 'intact' : 'CORRUPT', "\n";
        $c->close();
    } catch (Throwable $e) {
        echo "client error: ", $e->getMessage(), "\n";
    }
});

$server->start();
$returned = true;
await($client);
echo "done\n";
?>
--EXPECT--
window refilled after start() returned: yes
goaway: yes
end_stream: yes
bytes: 262144 of 262144, intact
done
