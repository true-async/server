--TEST--
HttpResponse — a buffered HTTP/2 response states the length of the body it is holding
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* DATA frames bound the body, so a peer can count it as it arrives. Two
 * questions they leave unanswered: how large the body of a HEAD response would
 * have been, and how large a download is before it finishes. RFC 9110 §8.6
 * asks for the field wherever the size is known before the header section is
 * sent, and says nothing about the version of the protocol.
 *
 * The count is the server's. A handler value that disagrees with the DATA is a
 * malformed message under RFC 9113 §8.1.1, so the one on /overstated is
 * replaced rather than forwarded. A 204 and an undeclared stream have no count
 * to state and send no field. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
);

$server->addHttpHandler(function ($req, $res) {
    switch ($req->getUri()) {
        case '/overstated':
            $res->setHeader('Content-Length', '999')->setBody('short')->end();
            return;

        case '/nocontent':
            $res->setStatusCode(204)->end();
            return;

        case '/streamed':
            $res->write('alpha');
            $res->end('beta');
            return;
    }

    $res->setHeader('Content-Type', 'text/plain')->setBody('payload')->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $run = static function (string $path, array $extra = []) use ($port) {
        $cmd = 'curl --http2-prior-knowledge -si --max-time 3 '
            . implode(' ', $extra)
            . sprintf(' http://127.0.0.1:%d%s 2>/dev/null', $port, $path);
        $out = (string) shell_exec($cmd);
        [$head, $body] = array_pad(preg_split("/\r\n\r\n/", $out, 2), 2, '');

        return [
            preg_match('/^HTTP\/2 (\d+)/', $head, $m) ? (int) $m[1] : -1,
            preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '-',
            strlen($body),
        ];
    };

    foreach ([
        ['buffered',   '/',           []],
        ['head',       '/',           ['-I']],
        ['overstated', '/overstated', []],
        ['nocontent',  '/nocontent',  []],
        ['streamed',   '/streamed',   []],
    ] as [$label, $path, $extra]) {
        [$status, $cl, $bytes] = $run($path, $extra);
        echo "$label: status=$status cl=$cl bytes=$bytes\n";
    }

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
buffered: status=200 cl=7 bytes=7
head: status=200 cl=7 bytes=0
overstated: status=200 cl=5 bytes=5
nocontent: status=204 cl=- bytes=0
streamed: status=200 cl=- bytes=9
Done
