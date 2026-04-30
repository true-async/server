<?php

/**
 * @generate-function-entries
 */

namespace TrueAsync;

/**
 * Parse HTTP request from string (for testing)
 *
 * @param string $request Raw HTTP request
 * @return HttpRequest|false Parsed request or false on error
 */
function http_parse_request(string $request): HttpRequest|false {}

/**
 * Dispose server internal state (for testing - prevents leak detector warnings)
 * Clears parser pool and all internal caches
 *
 * @return void
 */
function server_dispose(): void {}
