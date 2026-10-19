--TEST--
HTTP/2: a multipart form past max_input_vars is answered 400 and the handler sees HttpException 400 (#412)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif();
?>
--INI--
max_input_vars=5
--FILE--
<?php
/* Six fields against max_input_vars=5: the multipart processor refuses the
 * body on the sixth. The client reads the status (400) instead of a bare
 * stream error, and the handler that was reading the form is cancelled with
 * TrueAsync\HttpException whose code is that status (#412). The handler lets
 * the exception go, so the status is the server's, not the handler's. The
 * body follows the headers after a pause, so the handler is already parked in
 * getPost() when the refusal comes. h2/068 holds the same case through curl. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/_h2_client.inc';
require __DIR__ . '/_h2_refusal.inc';
require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$seen = 'handler did not run';

$server->addHttpHandler(function ($req, $res) use (&$seen) {
    try {
        $seen = 'form ' . json_encode($req->getPost());
    } catch (\Throwable $e) {
        $seen = get_class($e) . ' ' . $e->getCode();
        throw $e;
    }

    $res->setStatusCode(200)->setBody('read');
});

$body = implode('', array_map(
    static fn($i) => "--bnd\r\nContent-Disposition: form-data; name=\"k$i\"\r\n\r\nv\r\n", range(1, 6))) . "--bnd--\r\n";

$client = spawn(function () use ($port, $server, $body) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    $sid = $cli->sendRequest('POST', '/form', "127.0.0.1:$port",
        ['content-type' => 'multipart/form-data; boundary=bnd'], null, false);
    h2_ack_server_settings($cli);

    /* The handler is parked in getPost() when the body arrives. */
    delay(100);
    $cli->sendRawFrame(H2_FRAME_DATA, H2_FLAG_END_STREAM, $sid, $body);

    [$status, , , $ended] = $cli->collectResponse($sid);
    echo "status=$status\n";
    echo "ended=", (int)$ended, "\n";

    $cli->close();
    delay(200);
    $server->stop();
});

$server->start();
await($client);
echo "handler saw: $seen\n";
echo "done\n";
--EXPECT--
status=400
ended=1
handler saw: TrueAsync\HttpException 400
done
