/* Minimal stub so test_http3_packet can link src/http3/http3_packet.c in
 * isolation. http3_packet's version-negotiation / stateless-reset paths call
 * http3_listener_send_packet; the unit test only exercises the size-clamp /
 * refuse logic that runs BEFORE the send, so returning success (len) suffices.
 * This is the single undefined symbol of http3_packet.c outside the linked
 * OpenSSL / ngtcp2 / libphp libraries. */
#include <sys/types.h>
#include <sys/socket.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _http3_listener_s http3_listener_t;

ssize_t http3_listener_send_packet(http3_listener_t *listener,
                                   const void *buf, size_t len, uint8_t ecn,
                                   const struct sockaddr *peer,
                                   socklen_t peer_len)
{
    (void)listener; (void)buf; (void)ecn; (void)peer; (void)peer_len;
    return (ssize_t)len;
}
