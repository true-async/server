<?php
/* Shared helpers for the h2 tests of a request body the server refuses (#412). */

/* Read and ack the server SETTINGS, so the stream is open from both sides. */
function h2_ack_server_settings(H2TestClient $cli): void
{
    while (($fr = $cli->readFrame()) !== null) {
        [$type, $flags] = $fr;
        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $cli->sendSettingsAck();
            return;
        }
    }
}

/* Error code of the RST_STREAM the server sends for $sid, or null when none
 * arrives before the read timeout or the connection ends. Frames of other
 * kinds are skipped, so this reads past the end of a response. */
function h2_next_reset_code(H2TestClient $cli, int $sid): ?int
{
    $seen = $cli->lastResetCode();
    if ($seen !== null) {
        return $seen;
    }

    while (($fr = $cli->readFrame()) !== null) {
        [$type, , $fsid, $payload] = $fr;
        if ($type === H2_FRAME_RST_STREAM && $fsid === $sid) {
            return unpack('N', $payload)[1];
        }
    }

    return null;
}
