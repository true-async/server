--TEST--
HttpServer: request_context() works on the HTTP/3 dispatch path (per-request scope + subtree inheritance)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h3/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* The H3 per-stream dispatch (http3_dispatch.c) spawns its handler
 * coroutine through the same http_request_handler_coroutine_new helper
 * as H1/H2, so each stream gets its own per-request scope with
 * request_scope pointing at itself. This drives one end-to-end H3 GET
 * via the embedded h3client and asserts, inside the handler:
 *   - request_context() is non-null and IS current_context();
 *   - a child coroutine in a NESTED scope still resolves
 *     request_context() to the same per-request context (subtree
 *     inheritance), while its own current_context() differs.
 * The handler encodes the verdicts into the response body so the
 * single-shot client can assert them. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-046';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) {
    $cur = Async\current_context();
    $rc  = Async\request_context();
    $rc_null = ($rc === null) ? 1 : 0;
    if (!$rc_null) {
        $rc->set('rid', 'h3req');
    }

    // Child in a nested scope; hold the scope in a local so it is not
    // disposed before the child runs.
    $scope = \Async\Scope::inherit();
    $child = $scope->spawn(function () {
        $own  = Async\current_context();
        $reqc = Async\request_context();
        return [
            'rid'         => $reqc?->get('rid'),
            'own_differs' => ($own !== $reqc) ? 1 : 0,
        ];
    });
    $cd = await($child);

    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain; charset=utf-8')
        ->setBody(sprintf(
            "rc_null=%d handler_is_req=%d child_rid=%s child_own_differs=%d",
            $rc_null, ($cur === $rc) ? 1 : 0,
            $cd['rid'] ?? '(null)', $cd['own_differs']
        ));
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    usleep(80000);
    $cmd = sprintf('%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out  = shell_exec($cmd) ?? '';
    $body = trim(preg_replace('/^STATUS=\d+\n?/m', '', $out));
    echo "body=", $body, "\n";
    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
--EXPECT--
body=rc_null=0 handler_is_req=1 child_rid=h3req child_own_differs=1
