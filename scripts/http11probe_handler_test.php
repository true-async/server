<?php
/* Unit checks for the probe fixture's conditional response policy, without
 * requiring the extension. The actual handler is registered on these stubs. */
namespace TrueAsync {
    class HttpServerConfig {
        public function __call($method, $args) { return $this; }
    }
    class HttpServer {
        public static $handler;
        public function __construct($config) {}
        public function addHttpHandler($handler) { self::$handler = $handler; }
        public function start() {}
    }
}
namespace {
    require __DIR__ . '/../tests/bench/probe_server.php';
    class ProbeRequest {
        public function __construct(private $method, private $headers) {}
        public function getMethod() { return $this->method; }
        public function getHeader($name) { return $this->headers[$name] ?? null; }
        public function awaitBody() {}
        public function getBody() { return ''; }
    }
    class ProbeResponse {
        public $status;
        public function setStatusCode($status) { $this->status = $status; return $this; }
        public function setHeader($name, $value) { return $this; }
        public function setBody($value) { return $this; }
    }
    $cases = [
        ['GET', [], 200],
        ['GET', ['if-none-match' => PROBE_ETAG], 304],
        ['HEAD', ['if-none-match' => 'W/' . PROBE_ETAG], 304],
        ['POST', ['if-none-match' => '*'], 412],
        ['POST', ['if-none-match' => PROBE_ETAG], 412],
        ['GET', ['if-none-match' => '"different", W/' . PROBE_ETAG], 304],
        ['GET', ['if-none-match' => 'junk' . PROBE_ETAG], 200],
        ['GET', ['if-none-match' => '"probe-static-extra"'], 200],
        ['GET', ['if-none-match' => '"different"', 'if-modified-since' => PROBE_LASTMOD], 200],
        ['GET', ['if-none-match' => PROBE_ETAG, 'if-modified-since' => 'invalid'], 304],
        ['GET', ['if-modified-since' => PROBE_LASTMOD], 304],
        ['HEAD', ['if-modified-since' => PROBE_LASTMOD], 304],
        ['POST', ['if-modified-since' => PROBE_LASTMOD], 200],
        ['GET', ['if-modified-since' => 'Thu, 01 Jan 2099 00:00:00 GMT'], 304],
        ['GET', ['if-modified-since' => 'Wed, 21 Oct 2009 07:28:00 GMT'], 200],
        ['GET', ['if-modified-since' => 'tomorrow'], 200],
        ['GET', ['if-modified-since' => 'Wed, 31 Feb 2099 07:28:00 GMT'], 200],
        ['GET', ['if-modified-since' => PROBE_LASTMOD . ', ' . PROBE_LASTMOD], 200],
        ['GET', ['if-modified-since' => 'Wednesday, 21-Oct-15 07:28:00 GMT'], 304],
        ['GET', ['if-modified-since' => 'Wed Oct 21 07:28:00 2015'], 304],
    ];
    foreach ($cases as $index => [$method, $headers, $expected]) {
        $response = new ProbeResponse;
        (TrueAsync\HttpServer::$handler)(new ProbeRequest($method, $headers), $response);
        if ($response->status !== $expected) {
            throw new RuntimeException("case $index: expected $expected, got {$response->status}");
        }
    }
    echo count($cases), " handler checks passed\n";
}
