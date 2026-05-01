<?php
/**
 * TrueAsync Server demo.
 *
 *   php -d extension=./modules/true_async_server.so demo-server.php
 *
 * Then in another terminal:
 *   curl -v http://127.0.0.1:8080/
 *   curl -v http://127.0.0.1:8080/hello?name=Edmond
 *   curl -v -X POST -d 'payload' http://127.0.0.1:8080/echo
 *   curl -v http://127.0.0.1:8080/json
 *   curl -v http://127.0.0.1:8080/stop      # graceful shutdown
 *
 * Press Ctrl+C to exit.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;

$host = '0.0.0.0';
$port = 8080;

$config = (new HttpServerConfig())
    ->addListener($host, $port)
    ->setReadTimeout(30)
    ->setWriteTimeout(30)
    // Built-in logger: writes events directly from the C core to the
    // given php_stream. Severity is fixed at start() — change DEBUG
    // to INFO / WARN / ERROR for less verbose output, or LogSeverity::OFF
    // to disable.
    ->setLogSeverity(LogSeverity::DEBUG)
    ->setLogStream(STDOUT);

$server = new HttpServer($config);

$server->addHttpHandler(function ($request, $response) use ($server) {
    $method = $request->getMethod();
    $uri    = $request->getUri();
    $path   = parse_url($uri, PHP_URL_PATH) ?? '/';

    fprintf(STDERR, "[%s] %s %s\n", date('H:i:s'), $method, $uri);

    switch ($path) {
        case '/':
            $response
                ->setStatusCode(200)
                ->setHeader('Content-Type', 'text/html; charset=utf-8')
                ->setBody(<<<HTML
                    <!doctype html>
                    <html><head><title>TrueAsync Server</title></head>
                    <body>
                      <h1>It works!</h1>
                      <p>Served by <code>true_async_server</code> directly from PHP.</p>
                      <ul>
                        <li><a href="/hello?name=World">/hello?name=World</a></li>
                        <li><a href="/json">/json</a></li>
                        <li><code>POST /echo</code> &mdash; echoes the request body</li>
                        <li><a href="/stop">/stop</a> &mdash; shuts the server down</li>
                      </ul>
                    </body></html>
                    HTML)
                ->end();
            return;

        case '/hello':
            parse_str(parse_url($uri, PHP_URL_QUERY) ?? '', $q);
            $name = $q['name'] ?? 'stranger';
            $response
                ->setStatusCode(200)
                ->setHeader('Content-Type', 'text/plain; charset=utf-8')
                ->setBody("Hello, {$name}!\n")
                ->end();
            return;

        case '/json':
            $response
                ->setStatusCode(200)
                ->setHeader('Content-Type', 'application/json')
                ->setBody(json_encode([
                    'server'  => 'true_async_server',
                    'pid'     => getmypid(),
                    'time'    => date(DATE_ATOM),
                    'method'  => $method,
                    'uri'     => $uri,
                ], JSON_PRETTY_PRINT) . "\n")
                ->end();
            return;

        case '/echo':
            $body = method_exists($request, 'getBody') ? (string)$request->getBody() : '';
            $response
                ->setStatusCode(200)
                ->setHeader('Content-Type', 'text/plain')
                ->setBody("You sent {$method} with " . strlen($body) . " bytes:\n{$body}\n")
                ->end();
            return;

        case '/stop':
            $response
                ->setStatusCode(200)
                ->setHeader('Content-Type', 'text/plain')
                ->setBody("bye\n")
                ->end();
            $server->stop();
            return;

        default:
            $response
                ->setStatusCode(404)
                ->setHeader('Content-Type', 'text/plain')
                ->setBody("404 Not Found: {$path}\n")
                ->end();
    }
});

echo "TrueAsync Server listening on http://{$host}:{$port}\n";
echo "Try:  curl -v http://127.0.0.1:{$port}/\n";
echo "Stop: curl http://127.0.0.1:{$port}/stop  (or Ctrl+C)\n\n";

$server->start();

echo "Server stopped.\n";
