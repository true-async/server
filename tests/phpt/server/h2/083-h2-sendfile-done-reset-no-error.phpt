--TEST--
HTTP/2: a body past the cap after a sendFile() response is complete ends with RST_STREAM(NO_ERROR) (#412)
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
/* The handler answers with sendFile() and never reads the body, and the client
 * reads the whole response before it sends any DATA. The sendFile engine has
 * finished by then and dropped the response object: the answer is decided and
 * delivered all the same, so the refusal that follows ends the stream with
 * RST_STREAM(NO_ERROR) (code 0) and not with ENHANCE_YOUR_CALM (code 11). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/_h2_client.inc';
require __DIR__ . '/_h2_refusal.inc';
require_once __DIR__ . '/../_free_port.inc';

const BODY_CAP = 16 * 1024;
const CHUNK    = 8 * 1024;
const CHUNKS   = 4;

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setMaxBodySize(BODY_CAP));

$file = tempnam(sys_get_temp_dir(), 'h2sf_');
register_shutdown_function(static function () use ($file) {
    @unlink($file);
});
file_put_contents($file, 'sent whole');

$server->addHttpHandler(function ($req, $res) use ($file) {
    $res->sendFile($file);
});

$client = spawn(function () use ($port, $server) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    $sid = $cli->sendRequest('POST', '/file', "127.0.0.1:$port", [], null, false);
    h2_ack_server_settings($cli);

    /* The whole response arrives while the request body is still open. */
    [$status, $body, , $ended] = $cli->collectResponse($sid);
    echo "body=$body\n";
    echo "ended=", (int)$ended, "\n";

    for ($i = 0; $i < CHUNKS; $i++) {
        $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('a', CHUNK));
    }

    echo "reset_code=", h2_next_reset_code($cli, $sid) ?? 'none', "\n";

    $cli->close();
    delay(200);
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
body=sent whole
ended=1
reset_code=0
done
