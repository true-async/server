<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * Base exception for all HTTP server errors
 * @strict-properties
 */
class HttpServerException extends \Exception
{
}

/**
 * Runtime errors during server operation
 * @strict-properties
 */
final class HttpServerRuntimeException extends HttpServerException
{
}

/**
 * Invalid argument passed to server methods
 * @strict-properties
 */
final class HttpServerInvalidArgumentException extends HttpServerException
{
}

/**
 * Connection-related errors (socket, network)
 * @strict-properties
 */
final class HttpServerConnectionException extends HttpServerException
{
}

/**
 * Protocol-level errors (malformed HTTP, invalid headers)
 * @strict-properties
 */
final class HttpServerProtocolException extends HttpServerException
{
}

/**
 * Timeout errors (read, write, keep-alive)
 * @strict-properties
 */
final class HttpServerTimeoutException extends HttpServerException
{
}

/**
 * Throw from a handler to send a specific HTTP error response. The server
 * reads $code as the HTTP status (must be in 4xx/5xx, otherwise 500 is used)
 * and $message as the response body.
 *
 * Also raised internally when the parser hits a limit AFTER the handler
 * coroutine was dispatched: we cancel the handler with HttpException so the
 * cancellation propagates through the normal Async cancellation chain
 * (extends AsyncCancellation) while carrying the precise HTTP status to
 * send back to the peer.
 *
 * NOT marked final — user code may extend it for richer typing
 * (NotFoundException extends HttpException etc).
 */
class HttpException extends \Async\AsyncCancellation
{
}
