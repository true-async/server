--TEST--
HttpResponse::sendFile() — seals response: subsequent mutators throw
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpServerRuntimeException;
use function Async\spawn;
use function Async\await;

$tmp = tempnam(sys_get_temp_dir(), 'sf-');
file_put_contents($tmp, "ZZZZZ");

register_shutdown_function(function() use ($tmp) { @unlink($tmp); });

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($tmp) {
    $res->sendFile($tmp);
    /* Every mutating method must throw HttpServerRuntimeException. */
    $tries = [
        ['setHeader',     fn() => $res->setHeader('X-Foo', 'bar')],
        ['setStatusCode', fn() => $res->setStatusCode(201)],
        ['setReasonPhrase', fn() => $res->setReasonPhrase('Eh')],
        ['addHeader',     fn() => $res->addHeader('X', 'y')],
        ['resetHeaders',  fn() => $res->resetHeaders()],
        ['setBody',       fn() => $res->setBody('x')],
        ['write',         fn() => $res->write('x')],
        ['json',          fn() => $res->json(['a'=>1])],
        ['html',          fn() => $res->html('<p>')],
        ['redirect',      fn() => $res->redirect('/ok')],
        ['second-sendFile', fn() => $res->sendFile($GLOBALS['tmp'] ?? '/etc/hostname')],
        ['end',           fn() => $res->end()],
    ];
    foreach ($tries as [$name, $fn]) {
        try { $fn(); echo "$name: NO-THROW\n"; }
        catch (HttpServerRuntimeException $e) { echo "$name: throw\n"; }
    }
});

$client = spawn(function() use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    $resp = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $resp .= $c;
    }
    fclose($fp);
    $head_end = strpos($resp, "\r\n\r\n");
    $head = substr($resp, 0, $head_end);
    $body = substr($resp, $head_end + 4);
    $lines = explode("\r\n", $head);
    echo "status: ", $lines[0], "\n";
    echo "body: ", $body, "\n";
    $server->stop();
});
$server->start();
$GLOBALS['tmp'] = $tmp;
await($client);
echo "done\n";
--EXPECTF--
setHeader: throw
setStatusCode: throw
setReasonPhrase: throw
addHeader: throw
resetHeaders: throw
setBody: throw
write: throw
json: throw
html: throw
redirect: throw
second-sendFile: throw
end: throw
status: HTTP/1.1 200 OK
body: ZZZZZ
done
