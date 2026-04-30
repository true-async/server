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
    case Normal              = 1000;  /* normal closure */
    case GoingAway           = 1001;  /* server going down / client navigating away */
    case ProtocolError       = 1002;  /* protocol error */
    case UnsupportedData     = 1003;  /* received data of an unsupported type */
    case NoStatus            = 1005;  /* RESERVED — no status code in close frame */
    case AbnormalClosure     = 1006;  /* RESERVED — connection closed without close frame */
    case InvalidFramePayload = 1007;  /* received non-UTF-8 in a text message */
    case PolicyViolation     = 1008;  /* policy violation */
    case MessageTooBig       = 1009;  /* message too large to process */
    case MandatoryExtension  = 1010;  /* expected extension was not negotiated */
    case InternalServerError = 1011;  /* unexpected server error */
    case TlsHandshake        = 1015;  /* RESERVED — TLS handshake failure */
}
