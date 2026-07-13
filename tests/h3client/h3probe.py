"""Independent HTTP/3 probe (aioquic) for the phpt suite.

h3client shares ngtcp2 + nghttp3 with the server, so a shared misreading of the
spec would hide a bug from both. aioquic is a separate implementation, which is
the whole point of running some tests through it.

It prints one line the test can assert on:

    status=<code> bytes=<n> outcome=CLEAN_END|RESET(err=<code>)|TIMEOUT

`outcome` is what h3client could not tell apart until it learned to report
RESET_STREAM: a transfer the server aborts mid-body is NOT a short success.

Usage: python3 h3probe.py <host> <port> <path>
"""
import asyncio, sys
from aioquic.asyncio.client import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.quic.configuration import QuicConfiguration
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import HeadersReceived, DataReceived
from aioquic.quic.events import StreamReset

class Probe(QuicConnectionProtocol):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.h3 = H3Connection(self._quic)
        self.done = asyncio.get_event_loop().create_future()
        self.bytes = 0
        self.status = None
        self.outcome = None

    def get(self, host, path):
        sid = self._quic.get_next_available_stream_id()
        self.h3.send_headers(sid, [
            (b":method", b"GET"), (b":scheme", b"https"),
            (b":authority", host.encode()), (b":path", path.encode())], end_stream=True)
        self.transmit()

    def quic_event_received(self, event):
        if isinstance(event, StreamReset):
            self._finish("RESET(err=%d)" % event.error_code)
            return
        for e in self.h3.handle_event(event):
            if isinstance(e, HeadersReceived):
                self.status = dict(e.headers).get(b":status", b"?").decode()
            elif isinstance(e, DataReceived):
                self.bytes += len(e.data)
                if e.stream_ended:
                    self._finish("CLEAN_END")

    def _finish(self, outcome):
        if not self.done.done():
            self.outcome = outcome
            self.done.set_result(True)

async def main(host, port, path):
    cfg = QuicConfiguration(is_client=True, alpn_protocols=["h3"], verify_mode=0)
    async with connect(host, port, configuration=cfg, create_protocol=Probe) as p:
        p.get(host, path)
        try:
            await asyncio.wait_for(p.done, 20)
        except asyncio.TimeoutError:
            p.outcome = "TIMEOUT"
        print(f"status={p.status} bytes={p.bytes} outcome={p.outcome}")

asyncio.run(main(sys.argv[1], int(sys.argv[2]), sys.argv[3]))
