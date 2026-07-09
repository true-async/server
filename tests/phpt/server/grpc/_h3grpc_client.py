#!/usr/bin/env python3
"""HTTP/3 gRPC test client (aioquic) for the grpc-over-H3 phpt.

The bundled C h3client can't read HTTP/3 trailers, so gRPC-over-H3 is driven
with aioquic — the same QUIC stack the hq-interop tests use. Sends one POST
with a caller-supplied content-type + hex body, then prints every response
header AND trailer ("HDR name: value") plus the body hex ("BODYHEX ..."), so
the test can assert grpc-status.

  usage: _h3grpc_client.py <host> <port> <path> <ctype> <body-hex>

The body argument may be @/path/to/file to read the hex from a file
(large bodies exceed the argv limit).
"""
import asyncio
import ssl
import sys

from aioquic.asyncio import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.quic.configuration import QuicConfiguration
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import HeadersReceived, DataReceived


class H3Client(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.http = None
        self.done = asyncio.Event()
        self.headers = []   # initial headers AND trailers, in arrival order
        self.body = b""

    def quic_event_received(self, event):
        if self.http is None:
            self.http = H3Connection(self._quic)
        for e in self.http.handle_event(event):
            if isinstance(e, HeadersReceived):
                for k, v in e.headers:
                    self.headers.append((k.decode("ascii", "replace"),
                                         v.decode("ascii", "replace")))
                if e.stream_ended:
                    self.done.set()
            elif isinstance(e, DataReceived):
                self.body += e.data
                if e.stream_ended:
                    self.done.set()


async def main(host, port, path, ctype, body_hex):
    if body_hex.startswith("@"):
        with open(body_hex[1:]) as f:
            body_hex = f.read().strip()
    config = QuicConfiguration(is_client=True, alpn_protocols=["h3"])
    config.verify_mode = ssl.CERT_NONE
    async with connect(host, int(port), configuration=config,
                       create_protocol=H3Client) as client:
        if client.http is None:
            client.http = H3Connection(client._quic)
        sid = client._quic.get_next_available_stream_id()
        client.http.send_headers(sid, [
            (b":method", b"POST"),
            (b":scheme", b"https"),
            (b":authority", host.encode()),
            (b":path", path.encode()),
            (b"content-type", ctype.encode()),
            (b"te", b"trailers"),
        ], end_stream=False)
        client.http.send_data(sid, bytes.fromhex(body_hex), end_stream=True)
        client.transmit()
        try:
            await asyncio.wait_for(client.done.wait(), timeout=5)
        except asyncio.TimeoutError:
            print("TIMEOUT", file=sys.stderr)
        for k, v in client.headers:
            print(f"HDR {k}: {v}")
        print("BODYHEX " + client.body.hex())


if __name__ == "__main__":
    asyncio.run(main(*sys.argv[1:6]))
