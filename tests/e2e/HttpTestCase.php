<?php
/**
 * HttpTestCase - E2E test helper for HTTP server
 *
 * Usage:
 *   $test = new HttpTestCase();
 *   $test->serverHandler(fn($req, $res) => $res->setBody('Hello'));
 *   $test->sendRequest("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
 *   $test->expectStatus(200);
 *   $test->expectBody('Hello');
 *   $test->run();
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

class HttpTestCase
{
    private int $port;
    /** @var callable|null */
    private $handler = null;
    private string $request = '';
    private array $requests = [];  // For multiple requests (keep-alive)

    // Expectations
    private ?int $expectedStatus = null;
    private array $expectedHeaders = [];
    private ?string $expectedBody = null;
    private ?string $expectedRawResponse = null;
    /** @var callable|null */
    private $customValidator = null;

    // Results
    private string $rawResponse = '';
    private int $actualStatus = 0;
    private array $actualHeaders = [];
    private string $actualBody = '';

    // Timeouts
    private int $connectTimeout = 2;
    private int $readTimeout = 5;
    private int $serverTimeout = 5;

    public function __construct(int $port = 0)
    {
        // Auto-assign port based on PID to avoid conflicts
        $this->port = $port ?: (19800 + getmypid() % 1000);
    }

    /**
     * Set server request handler
     */
    public function serverHandler(callable $handler): self
    {
        $this->handler = $handler;
        return $this;
    }

    /**
     * Set raw HTTP request to send
     */
    public function sendRequest(string $request): self
    {
        $this->request = $request;
        $this->requests = [$request];
        return $this;
    }

    /**
     * Set multiple requests for keep-alive testing
     */
    public function sendRequests(array $requests): self
    {
        $this->requests = $requests;
        $this->request = implode('', $requests);
        return $this;
    }

    /**
     * Expect specific HTTP status code
     */
    public function expectStatus(int $status): self
    {
        $this->expectedStatus = $status;
        return $this;
    }

    /**
     * Expect specific header value
     */
    public function expectHeader(string $name, string $value): self
    {
        $this->expectedHeaders[strtolower($name)] = $value;
        return $this;
    }

    /**
     * Expect specific body content
     */
    public function expectBody(string $body): self
    {
        $this->expectedBody = $body;
        return $this;
    }

    /**
     * Expect exact raw response (for precise testing)
     */
    public function expectResponse(string $response): self
    {
        $this->expectedRawResponse = $response;
        return $this;
    }

    /**
     * Custom validation callback
     * Receives: (int $status, array $headers, string $body, string $rawResponse)
     */
    public function validate(callable $validator): self
    {
        $this->customValidator = $validator;
        return $this;
    }

    /**
     * Set timeouts
     */
    public function setTimeout(int $connect = 2, int $read = 5, int $server = 5): self
    {
        $this->connectTimeout = $connect;
        $this->readTimeout = $read;
        $this->serverTimeout = $server;
        return $this;
    }

    /**
     * Run the test
     */
    public function run(): void
    {
        if (!$this->handler) {
            throw new \RuntimeException('No server handler defined. Use serverHandler()');
        }

        if (empty($this->request)) {
            throw new \RuntimeException('No request defined. Use sendRequest()');
        }

        $config = (new HttpServerConfig())
            ->addListener('127.0.0.1', $this->port)
            ->setReadTimeout($this->serverTimeout)
            ->setWriteTimeout($this->serverTimeout);

        $server = new HttpServer($config);
        $handler = $this->handler;
        $requestCount = count($this->requests);
        $handledCount = 0;

        $server->addHttpHandler(function($request, $response) use ($handler, $server, &$handledCount, $requestCount) {
            $handler($request, $response);
            $handledCount++;

            // Stop after handling all expected requests
            if ($handledCount >= $requestCount) {
                $server->stop();
            }
        });

        // Spawn client coroutine; hold the Coroutine handle so we can await it
        $testCase = $this;
        $clientCoroutine = spawn(function() use ($testCase) {
            usleep(10000);  // Let server start
            $testCase->executeClient();
        });

        // Start server (blocks until stop())
        $server->start();

        // Wait for the client coroutine to finish before validating — otherwise
        // rawResponse may still be empty if validation runs before the client
        // finishes reading. The main coroutine yields control here.
        try {
            await($clientCoroutine);
        } catch (\Throwable $e) {
            // Client errors are surfaced via validateResults() below when
            // rawResponse is empty / status is 0.
        }

        // Validate results
        $this->validateResults();
    }

    /**
     * Execute HTTP client
     */
    public function executeClient(): void
    {
        $fp = @stream_socket_client(
            "tcp://127.0.0.1:{$this->port}",
            $errno,
            $errstr,
            $this->connectTimeout
        );

        if (!$fp) {
            throw new \RuntimeException("Client connect failed: $errstr ($errno)");
        }

        stream_set_timeout($fp, $this->readTimeout);

        // Send request
        fwrite($fp, $this->request);

        // Read response
        $this->rawResponse = '';
        while (!feof($fp)) {
            $chunk = fread($fp, 8192);
            if ($chunk === false) break;
            $this->rawResponse .= $chunk;
        }

        fclose($fp);

        // Parse response
        $this->parseResponse();
    }

    /**
     * Parse raw HTTP response
     */
    private function parseResponse(): void
    {
        $parts = explode("\r\n\r\n", $this->rawResponse, 2);
        $headerSection = $parts[0] ?? '';
        $this->actualBody = $parts[1] ?? '';

        $lines = explode("\r\n", $headerSection);
        $statusLine = array_shift($lines);

        // Parse status line: HTTP/1.1 200 OK
        if (preg_match('/^HTTP\/\d+\.\d+\s+(\d+)/', $statusLine, $matches)) {
            $this->actualStatus = (int)$matches[1];
        }

        // Parse headers
        $this->actualHeaders = [];
        foreach ($lines as $line) {
            if (strpos($line, ':') !== false) {
                [$name, $value] = explode(':', $line, 2);
                $this->actualHeaders[strtolower(trim($name))] = trim($value);
            }
        }
    }

    /**
     * Validate test results
     */
    private function validateResults(): void
    {
        $errors = [];

        // Check status
        if ($this->expectedStatus !== null && $this->actualStatus !== $this->expectedStatus) {
            $errors[] = "Status mismatch: expected {$this->expectedStatus}, got {$this->actualStatus}";
        }

        // Check headers
        foreach ($this->expectedHeaders as $name => $value) {
            $actual = $this->actualHeaders[$name] ?? null;
            if ($actual !== $value) {
                $errors[] = "Header '$name' mismatch: expected '$value', got '" . ($actual ?? 'null') . "'";
            }
        }

        // Check body
        if ($this->expectedBody !== null && $this->actualBody !== $this->expectedBody) {
            $errors[] = "Body mismatch:\n  Expected: {$this->expectedBody}\n  Actual: {$this->actualBody}";
        }

        // Check raw response
        if ($this->expectedRawResponse !== null) {
            $normalized = $this->normalizeLineEndings($this->expectedRawResponse);
            $actualNormalized = $this->normalizeLineEndings($this->rawResponse);
            if ($actualNormalized !== $normalized) {
                $errors[] = "Raw response mismatch:\n--- Expected ---\n{$this->expectedRawResponse}\n--- Actual ---\n{$this->rawResponse}";
            }
        }

        // Custom validator
        if ($this->customValidator) {
            $result = ($this->customValidator)(
                $this->actualStatus,
                $this->actualHeaders,
                $this->actualBody,
                $this->rawResponse
            );
            if ($result !== true && $result !== null) {
                $errors[] = is_string($result) ? $result : "Custom validation failed";
            }
        }

        if (!empty($errors)) {
            throw new \RuntimeException("Test failed:\n" . implode("\n", $errors));
        }

        echo "OK\n";
    }

    /**
     * Normalize line endings for comparison
     */
    private function normalizeLineEndings(string $str): string
    {
        return str_replace(["\r\n", "\r"], "\n", trim($str));
    }

    /**
     * Get actual response (for debugging)
     */
    public function getResponse(): array
    {
        return [
            'status' => $this->actualStatus,
            'headers' => $this->actualHeaders,
            'body' => $this->actualBody,
            'raw' => $this->rawResponse,
        ];
    }

    /**
     * Print actual response (for debugging)
     */
    public function dumpResponse(): void
    {
        echo "=== Response ===\n";
        echo "Status: {$this->actualStatus}\n";
        echo "Headers:\n";
        foreach ($this->actualHeaders as $name => $value) {
            echo "  $name: $value\n";
        }
        echo "Body: {$this->actualBody}\n";
        echo "=== Raw ===\n{$this->rawResponse}\n";
    }
}
