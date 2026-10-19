--TEST--
HTTP/2: a refused body does not cut a sendFile() response in flight, the stream ends with RST_STREAM(NO_ERROR) (#412)
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
/* The handler calls sendFile() on a 1 MiB file and never reads the request
 * body. The client's 65535-byte stream window stops the server after the first
 * DATA frames, so the response is decided and still in flight when the client
 * sends DATA past the 16 KiB cap. The file must arrive whole, END_STREAM
 * included, and only then is the stream reset, with NO_ERROR. Before #412 the
 * refusal reset it with ENHANCE_YOUR_CALM (code 11) in the middle of the file. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/_h2_client.inc';
require __DIR__ . '/_h2_refusal.inc';
require_once __DIR__ . '/../_free_port.inc';

const BODY_CAP  = 16 * 1024;
const CHUNK     = 8 * 1024;
const CHUNKS    = 4;
const FILE_SIZE = 1024 * 1024;

$file = tempnam(sys_get_temp_dir(), 'h2sf_');
register_shutdown_function(static function () use ($file) {
    @unlink($file);
});
file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOP', FILE_SIZE / 16));

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setMaxBodySize(BODY_CAP));

$server->addHttpHandler(function ($req, $res) use ($file) {
    $res->sendFile($file);
});

$client = spawn(function () use ($port, $server) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    $sid = $cli->sendRequest('POST', '/file', "127.0.0.1:$port", [], null, false);

    /* Read up to the first response DATA frame: from here the response is on
     * the wire and the server waits for window credit. */
    $first = 0;
    while (($fr = $cli->readFrame()) !== null) {
        [$type, $flags, $fsid, $payload] = $fr;
        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $cli->sendSettingsAck();
        } elseif ($type === H2_FRAME_DATA && $fsid === $sid) {
            $first = strlen($payload);
            break;
        }
    }
    echo "response started: ", (int)($first > 0), "\n";

    for ($i = 0; $i < CHUNKS; $i++) {
        $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('a', CHUNK));
    }

    [$status, $body, , $ended] = $cli->collectResponse($sid);
    echo "received=", $first + strlen($body), " of ", FILE_SIZE, "\n";
    echo "ended=", (int)$ended, "\n";
    echo "reset_code=", h2_next_reset_code($cli, $sid) ?? 'none', "\n";

    $cli->close();
    delay(200);
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
response started: 1
received=1048576 of 1048576
ended=1
reset_code=0
done
