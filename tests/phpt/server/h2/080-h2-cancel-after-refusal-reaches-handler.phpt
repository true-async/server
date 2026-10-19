--TEST--
HTTP/2: a client reset after a refused body still reaches the handler as HttpException 499 (#412)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif();
?>
--FILE--
<?php
/* The handler catches the refusal (HttpException 413) and answers itself with
 * a streamed 200, a chunk every 50 ms. Since #412 that response reaches the
 * client: the refusal no longer resets the stream while no response is
 * decided. Once the first chunk arrives the client resets the stream with
 * CANCEL, and the handler must hear that as a second HttpException, code 499
 * (client gone), not as the refusal again. Before #412 the stream was reset
 * at the refusal and the client read no status. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/_h2_client.inc';
require __DIR__ . '/_h2_refusal.inc';
require_once __DIR__ . '/../_free_port.inc';

const BODY_CAP = 16 * 1024;
const CHUNK    = 8 * 1024;
const CHUNKS   = 4;
const CANCEL   = 8;

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setMaxBodySize(BODY_CAP));

$codes = [];

$server->addHttpHandler(function ($req, $res) use (&$codes) {
    try {
        $req->awaitBody();
    } catch (HttpException $e) {
        $codes[] = $e->getCode();
    }

    try {
        $res->setStatusCode(200);
        for ($i = 0; $i < 100; $i++) {
            $res->write('chunk');
            delay(50);
        }
    } catch (HttpException $e) {
        $codes[] = $e->getCode();
    } catch (\Throwable $e) {
        $codes[] = get_class($e);
    }
});

$client = spawn(function () use ($port, $server) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    $sid = $cli->sendRequest('POST', '/cancel', "127.0.0.1:$port", [], null, false);
    h2_ack_server_settings($cli);

    delay(100);

    for ($i = 0; $i < CHUNKS; $i++) {
        $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('a', CHUNK));
    }

    /* The handler's own HEADERS, then its first chunk. */
    $status = 0;
    $chunk  = false;
    while (!$chunk && ($fr = $cli->readFrame()) !== null) {
        [$type, $flags, $fsid, $payload] = $fr;
        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $cli->sendSettingsAck();
            continue;
        }
        if ($fsid !== $sid) {
            continue;
        }
        if ($type === H2_FRAME_HEADERS) {
            $status = H2TestClient::statusOf($payload);
        } elseif ($type === H2_FRAME_DATA) {
            $chunk = true;
        } elseif ($type === H2_FRAME_RST_STREAM) {
            break;
        }
    }
    echo "status=$status\n";
    echo "first chunk: ", (int)$chunk, "\n";

    $cli->sendRstStream($sid, CANCEL);
    delay(500);

    $cli->close();
    $server->stop();
});

$server->start();
await($client);
echo "handler codes: ", implode(',', $codes), "\n";
echo "done\n";
--EXPECT--
status=200
first chunk: 1
handler codes: 413,499
done
