<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * Logger severity levels.
 *
 * Backing values match the OpenTelemetry Logs Data Model SeverityNumber
 * (1-24). Only OFF + a stable subset of OTel buckets are exposed here:
 *
 *   - DEBUG (5)  — verbose, diagnostic-only output (h3 packet trace, etc.)
 *   - INFO  (9)  — server lifecycle (start/stop), bind retries
 *   - WARN  (13) — TLS handshake fail, peer reset, absorbed exceptions
 *   - ERROR (17) — listener bind failed, hard protocol error
 *
 * TRACE / FATAL are intentionally absent — TRACE is unused, FATAL is
 * delivered via zend_error_noreturn(E_ERROR) which already terminates.
 */
enum LogSeverity: int
{
    case OFF   = 0;
    case DEBUG = 5;
    case INFO  = 9;
    case WARN  = 13;
    case ERROR = 17;
}
