<?php

/**
 * @generate-class-entries
 */

namespace TrueAsync;

/**
 * RFC 6455 §7.4.1 close-code registry. Application-specific codes
 * (4000-4999, RFC 6455 §7.4.2) stay open via WebSocket::close()
 * accepting `int` alongside this enum — the IANA registered codes
 * here cover every standard scenario.
 */
enum WebSocketCloseCode: int
{
    case NORMAL                = 1000;  /* normal closure */
    case GOING_AWAY            = 1001;  /* server going down / client navigating away */
    case PROTOCOL_ERROR        = 1002;  /* protocol error */
    case UNSUPPORTED_DATA      = 1003;  /* received data of an unsupported type */
    case NO_STATUS             = 1005;  /* RESERVED — no status code in close frame */
    case ABNORMAL_CLOSURE      = 1006;  /* RESERVED — connection closed without close frame */
    case INVALID_FRAME_PAYLOAD = 1007;  /* received non-UTF-8 in a text message */
    case POLICY_VIOLATION      = 1008;  /* policy violation */
    case MESSAGE_TOO_BIG       = 1009;  /* message too large to process */
    case MANDATORY_EXTENSION   = 1010;  /* expected extension was not negotiated */
    case INTERNAL_SERVER_ERROR = 1011;  /* unexpected server error */
    case TLS_HANDSHAKE         = 1015;  /* RESERVED — TLS handshake failure */
}
