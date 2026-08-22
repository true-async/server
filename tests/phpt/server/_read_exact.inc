<?php
/* Read a body framed by a byte count off a raw socket.
 *
 * fread() on a network stream returns as soon as one segment is available, so a
 * body the server sent as two writes comes back short: five bytes of the nine a
 * handler wrote as "alpha" then "beta" under Content-Length: 9. The count is the
 * framing, so the read continues until it is met.
 *
 * Returns what arrived, which is shorter than $len when the peer closed or the
 * stream timed out, so a caller comparing against the whole body sees the
 * shortfall instead of hanging on it. */
function tas_read_exact($fp, int $len): string
{
    $body = '';

    while (strlen($body) < $len) {
        $piece = fread($fp, $len - strlen($body));

        if ($piece === false || $piece === '') {
            break;
        }

        $body .= $piece;
    }

    return $body;
}
