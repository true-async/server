/* Minimal stub so test_http3_packet can link src/http3/http3_packet.c in
 * isolation. http3_packet's version-negotiation / stateless-reset paths call
 * http3_listener_send_packet; the stub keeps the last packet so the test can
 * read what would have gone on the wire, and reports it sent.
 * This is the single undefined symbol of http3_packet.c outside the linked
 * OpenSSL / ngtcp2 / libphp libraries. */
#include <sys/types.h>
#include <sys/socket.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _http3_listener_s http3_listener_t;

#include <string.h>

/* The last packet sent, for the test to inspect: its first 1500 bytes and its
 * full length. */
uint8_t http3_stub_last_packet[1500];
size_t  http3_stub_last_packet_len;

ssize_t http3_listener_send_packet(http3_listener_t *listener,
                                   const void *buf, size_t len, uint8_t ecn,
                                   const struct sockaddr *peer,
                                   socklen_t peer_len)
{
    (void)listener; (void)ecn; (void)peer; (void)peer_len;
    memcpy(http3_stub_last_packet, buf,
           len < sizeof(http3_stub_last_packet) ? len : sizeof(http3_stub_last_packet));
    http3_stub_last_packet_len = len;
    return (ssize_t)len;
}
