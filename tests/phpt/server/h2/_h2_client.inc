<?php
/*
 * Minimal HTTP/2 (h2c, prior-knowledge) client for phpt tests.
 *
 * Purpose: drive the server with exact wire-level control — notably,
 * send WINDOW_UPDATE on demand to break curl's quirk of under-
 * crediting stream windows. Not a production client; skips HPACK
 * dynamic table, skips Huffman coding, skips priority, skips TLS.
 *
 * Assumes prior-knowledge h2c: client speaks HTTP/2 from the first
 * byte. Server (our impl) advertises 1 MiB connection window; we
 * advertise defaults (64 KiB stream initial). Client is single-
 * request-per-instance — instantiate fresh per test scenario.
 *
 * HPACK encoding strategy: every outgoing header uses "literal with
 * literal name, never indexed" (0x00 prefix byte, then name and
 * value as length-prefixed strings, no Huffman). Simple, universally
 * accepted by nghttp2.
 *
 * HPACK parsing on the response: minimal — we decode the first
 * byte of each header entry to recognise static-table :status
 * indices (8..14 cover common 200/204/206/304/400/404/500), and
 * otherwise treat the entry as a literal with indexed name or
 * literal name and skip over its bytes. Enough to detect status
 * and accumulate DATA bodies.
 */

const H2_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

const H2_FRAME_DATA          = 0x00;
const H2_FRAME_HEADERS       = 0x01;
const H2_FRAME_PRIORITY      = 0x02;
const H2_FRAME_RST_STREAM    = 0x03;
const H2_FRAME_SETTINGS      = 0x04;
const H2_FRAME_PUSH_PROMISE  = 0x05;
const H2_FRAME_PING          = 0x06;
const H2_FRAME_GOAWAY        = 0x07;
const H2_FRAME_WINDOW_UPDATE = 0x08;
const H2_FRAME_CONTINUATION  = 0x09;

const H2_FLAG_END_STREAM   = 0x01;
const H2_FLAG_ACK          = 0x01;
const H2_FLAG_END_HEADERS  = 0x04;

class H2TestClient
{
    /** @var resource */
    private $sock;
    private int $next_stream_id = 1;  /* client streams are odd */

    /** @param int $timeout_sec read timeout */
    public function __construct(string $host, int $port, int $timeout_sec = 5)
    {
        $errno = 0; $errstr = '';
        $this->sock = stream_socket_client("tcp://$host:$port", $errno, $errstr, 3);
        if ($this->sock === false) {
            throw new RuntimeException("connect failed: $errstr ($errno)");
        }
        stream_set_timeout($this->sock, $timeout_sec);
        stream_set_blocking($this->sock, true);
        /* Disable PHP's user-space write buffer — otherwise small
         * frames like WINDOW_UPDATE (13 B) get held in libc stdio-
         * like buffer until the next bigger write or fclose. */
        stream_set_write_buffer($this->sock, 0);
        stream_set_read_buffer($this->sock, 0);

        /* Disable Nagle — small frames must reach the server
         * immediately or the backpressure test deadlocks. */
        if (function_exists('socket_import_stream')) {
            $sk = @socket_import_stream($this->sock);
            if ($sk !== false) {
                @socket_set_option($sk, SOL_TCP, TCP_NODELAY, 1);
            }
        }

        fwrite($this->sock, H2_PREFACE);
        /* Empty SETTINGS — use RFC defaults (initial window 65535). */
        $this->writeFrame(H2_FRAME_SETTINGS, 0, 0, '');
    }

    public function close(): void
    {
        if (is_resource($this->sock)) {
            fclose($this->sock);
        }
    }

    /* ----- Frame primitives ----- */

    private function writeFrame(int $type, int $flags, int $stream_id, string $payload): void
    {
        $len = strlen($payload);
        $hdr = chr(($len >> 16) & 0xff)
             . chr(($len >> 8)  & 0xff)
             . chr( $len        & 0xff)
             . chr($type)
             . chr($flags)
             . pack('N', $stream_id & 0x7fffffff);
        fwrite($this->sock, $hdr . $payload);
        fflush($this->sock);
    }

    /**
     * Read one frame. Returns [type, flags, stream_id, payload] or
     * null on EOF/timeout.
     */
    public function readFrame(): ?array
    {
        $hdr = $this->readExactly(9);
        if ($hdr === null) { return null; }
        $b = array_values(unpack('C9', $hdr));
        $len   = ($b[0] << 16) | ($b[1] << 8) | $b[2];
        $type  = $b[3];
        $flags = $b[4];
        $sid   = (($b[5] & 0x7f) << 24) | ($b[6] << 16) | ($b[7] << 8) | $b[8];
        $payload = $len > 0 ? $this->readExactly($len) : '';
        if ($payload === null) { return null; }
        return [$type, $flags, $sid, $payload];
    }

    private function readExactly(int $n): ?string
    {
        $buf = '';
        while (strlen($buf) < $n) {
            $chunk = fread($this->sock, $n - strlen($buf));
            $info  = stream_get_meta_data($this->sock);
            if ($chunk === false || $chunk === '' || $info['timed_out'] || $info['eof']) {
                return null;
            }
            $buf .= $chunk;
        }
        return $buf;
    }

    /* ----- HPACK minimal encode ----- */

    private static function hpackInt(int $value, int $prefix_bits): string
    {
        $max = (1 << $prefix_bits) - 1;
        if ($value < $max) {
            return chr($value);
        }
        $out = chr($max);
        $value -= $max;
        while ($value >= 128) {
            $out .= chr(($value & 0x7f) | 0x80);
            $value >>= 7;
        }
        $out .= chr($value);
        return $out;
    }

    private static function hpackString(string $s): string
    {
        /* Length (7-bit prefix, H=0 → no Huffman) + raw bytes. */
        return self::hpackInt(strlen($s), 7) . $s;
    }

    private static function hpackLiteralNeverIndexed(string $name, string $value): string
    {
        /* 0x00 = literal with literal name, without indexing.
         * Name is the low-cost unambiguous encoding that every HPACK
         * implementation (including nghttp2's strict mode) accepts. */
        return "\x00"
             . self::hpackString(strtolower($name))
             . self::hpackString($value);
    }

    /* ----- Public frame senders ----- */

    /**
     * Send a request and return the assigned stream id. headers is an
     * associative array; pseudo-headers `:method`/`:path`/`:scheme`/
     * `:authority` are added if missing. If $body === null, the
     * stream is half-closed immediately (GET-style); otherwise body
     * bytes go as a single DATA frame with END_STREAM.
     */
    public function sendRequest(
        string $method, string $path, string $authority,
        array $headers = [], ?string $body = null): int
    {
        $sid = $this->next_stream_id;
        $this->next_stream_id += 2;

        $hpack  = self::hpackLiteralNeverIndexed(':method',    $method);
        $hpack .= self::hpackLiteralNeverIndexed(':path',      $path);
        $hpack .= self::hpackLiteralNeverIndexed(':scheme',    'http');
        $hpack .= self::hpackLiteralNeverIndexed(':authority', $authority);
        foreach ($headers as $name => $value) {
            $hpack .= self::hpackLiteralNeverIndexed($name, (string)$value);
        }

        $flags = H2_FLAG_END_HEADERS;
        if ($body === null) { $flags |= H2_FLAG_END_STREAM; }
        $this->writeFrame(H2_FRAME_HEADERS, $flags, $sid, $hpack);

        if ($body !== null) {
            $this->writeFrame(H2_FRAME_DATA, H2_FLAG_END_STREAM, $sid, $body);
        }
        return $sid;
    }

    /**
     * Send a WINDOW_UPDATE frame. stream_id=0 for connection-level,
     * non-zero for stream-specific. Increment must be 1..2^31-1.
     */
    public function sendWindowUpdate(int $stream_id, int $increment): void
    {
        $payload = pack('N', $increment & 0x7fffffff);
        $this->writeFrame(H2_FRAME_WINDOW_UPDATE, 0, $stream_id, $payload);
    }

    public function sendSettingsAck(): void
    {
        $this->writeFrame(H2_FRAME_SETTINGS, H2_FLAG_ACK, 0, '');
    }

    public function sendRstStream(int $stream_id, int $error_code = 0): void
    {
        $this->writeFrame(H2_FRAME_RST_STREAM, 0, $stream_id,
                          pack('N', $error_code));
    }

    /* ----- High-level response collector ----- */

    /**
     * Drive the read loop, collecting body bytes for a single
     * stream. Auto-ACKs server SETTINGS. Auto-sends WINDOW_UPDATE if
     * $auto_window=true (default) whenever half the initial window
     * has been consumed — matches conformant-client behaviour. Set
     * $auto_window=false to test backpressure-deadlock scenarios.
     *
     * Returns: [status, body, trailers, ended_gracefully]
     *   - status: int HTTP status or 0 if not received
     *   - body:   concatenated DATA bytes
     *   - trailers: [] currently (not parsed in detail)
     *   - ended_gracefully: true when END_STREAM observed cleanly
     *
     * Stops when END_STREAM observed OR read timeout / socket close.
     */
    public function collectResponse(int $stream_id, bool $auto_window = true): array
    {
        $body = '';
        $status = 0;
        $trailers = [];
        $ended = false;
        $seen_any_headers = false;

        /* Aggressive refill — ack per 16 KiB consumed. Keeps window
         * from bunching up against frame boundaries; matches conformant
         * client pattern (curl-fixed, grpc-go, nghttp). */
        $consumed_since_ack = 0;
        $ack_threshold = 16 * 1024;

        while (true) {
            $fr = $this->readFrame();
            if ($fr === null) { break; }
            [$type, $flags, $sid, $payload] = $fr;

            if ($type === H2_FRAME_SETTINGS) {
                if (!($flags & H2_FLAG_ACK)) {
                    /* Server settings — ack. */
                    $this->sendSettingsAck();
                }
                continue;
            }
            if ($type === H2_FRAME_WINDOW_UPDATE) {
                continue;  /* server adjusting its receive window — ignore */
            }
            if ($type === H2_FRAME_GOAWAY) {
                break;     /* session done */
            }
            if ($type === H2_FRAME_PING) {
                if (!($flags & H2_FLAG_ACK)) {
                    /* Reflect payload with ACK bit set. */
                    $this->writeFrame(H2_FRAME_PING, H2_FLAG_ACK, 0, $payload);
                }
                continue;
            }

            /* From here only our stream matters. */
            if ($sid !== $stream_id) { continue; }

            if ($type === H2_FRAME_HEADERS) {
                if (!$seen_any_headers) {
                    $status = self::parseStatusFromHpackBlock($payload);
                    $seen_any_headers = true;
                } else {
                    /* Trailer HEADERS frame — we don't fully decode,
                     * but record that a trailer arrived. */
                    $trailers[] = true;
                }
                if ($flags & H2_FLAG_END_STREAM) {
                    $ended = true;
                    break;
                }
                continue;
            }

            if ($type === H2_FRAME_DATA) {
                $body .= $payload;
                $consumed_since_ack += strlen($payload);

                if ($auto_window && $consumed_since_ack >= $ack_threshold) {
                    /* Refill both stream + connection windows. */
                    $this->sendWindowUpdate($stream_id, $consumed_since_ack);
                    $this->sendWindowUpdate(0,           $consumed_since_ack);
                    $consumed_since_ack = 0;
                }

                if ($flags & H2_FLAG_END_STREAM) {
                    $ended = true;
                    break;
                }
                continue;
            }

            if ($type === H2_FRAME_RST_STREAM) {
                break;
            }
        }

        return [$status, $body, $trailers, $ended];
    }

    /**
     * Pull the status code out of an HPACK-encoded HEADERS payload.
     * Accepts the three shapes nghttp2 emits for :status:
     *   - indexed field (0x88..0x8e for static-table 200/204/206/
     *     304/400/404/500);
     *   - literal with indexed name (static index 8 = :status),
     *     followed by numeric value as a string;
     *   - literal with literal name (full ":status\0<digits>").
     * Not a full HPACK decoder — skips over every subsequent header
     * to stay correct when our :status is the first entry. */
    private static function parseStatusFromHpackBlock(string $payload): int
    {
        if ($payload === '') { return 0; }
        $b = ord($payload[0]);

        /* Indexed — 0b1xxxxxxx. */
        if ($b & 0x80) {
            $idx = $b & 0x7f;
            switch ($idx) {
                case 8:  return 200;
                case 9:  return 204;
                case 10: return 206;
                case 11: return 304;
                case 12: return 400;
                case 13: return 404;
                case 14: return 500;
            }
            return 0;  /* uncommon index, we don't track dynamic table */
        }

        /* Literal with indexed name (0b01xxxxxx / 0b0001xxxx /
         * 0b0000xxxx). We just need to skip the name (or index)
         * and read the value bytes. */
        $pos = 0;
        if (($b & 0xc0) === 0x40) {
            /* 0b01xxxxxx — 6-bit prefix index. */
            [$_index, $pos] = self::decodeInt($payload, 0, 6);
        } else {
            /* 0b0000xxxx or 0b0001xxxx — 4-bit prefix index. */
            [$_index, $pos] = self::decodeInt($payload, 0, 4);
        }

        /* If name index was 0, the name is a string literal. */
        $first_index = ($b & 0x0f);
        if (($b & 0xc0) === 0x40) { $first_index = $b & 0x3f; }
        if ($first_index === 0) {
            /* Name length byte + name. */
            [$nlen, $pos] = self::decodeInt($payload, $pos, 7);
            $pos += $nlen;
        }

        /* Value length byte + value. */
        [$vlen, $pos] = self::decodeInt($payload, $pos, 7);
        $value = substr($payload, $pos, $vlen);
        return (int)$value;
    }

    private static function decodeInt(string $buf, int $pos, int $prefix_bits): array
    {
        $max = (1 << $prefix_bits) - 1;
        $val = ord($buf[$pos]) & $max;
        $pos++;
        if ($val < $max) {
            return [$val, $pos];
        }
        $m = 0;
        while (true) {
            $b = ord($buf[$pos]);
            $pos++;
            $val += ($b & 0x7f) << $m;
            $m += 7;
            if (!($b & 0x80)) { break; }
        }
        return [$val, $pos];
    }
}
