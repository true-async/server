#!/usr/bin/env python3
"""hq-interop (HTTP/0.9-over-QUIC) test client for the H3 phpt suite.

The server speaks hq only on the QUIC ALPN "hq-interop" (no nghttp3), so the
nghttp3-based h3client harness can't drive it. aioquic is the QUIC stack the
quic-interop-runner itself uses, so it doubles as the reference hq client.

Modes:
  _hq_client.py HOST PORT /path
      Single bidi stream. Writes the raw response body to stdout (binary);
      the phpt sha-compares it against the served file.

  _hq_client.py HOST PORT --mux /p1 /p2 ...
      Opens all paths as concurrent bidi streams on ONE connection
      (multiplexing). Writes one "len sha1" line per path, in input order,
      to stdout. Proves N streams complete correctly on a single conn.

Status / errors go to stderr so stdout stays exactly the payload.
"""
import asyncio
import hashlib
import ssl
import sys

from aioquic.asyncio import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import (
    ConnectionTerminated,
    HandshakeCompleted,
    StreamDataReceived,
)


class HQ(QuicConnectionProtocol):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.bufs = {}            # stream_id -> bytearray
        self.fins = set()         # stream_ids that saw end_stream
        self.hs = asyncio.Event()
        self.closed = asyncio.Event()

    def quic_event_received(self, ev):
        if isinstance(ev, HandshakeCompleted):
            self.hs.set()
        elif isinstance(ev, StreamDataReceived):
            self.bufs.setdefault(ev.stream_id, bytearray()).extend(ev.data)
            if ev.end_stream:
                self.fins.add(ev.stream_id)
        elif isinstance(ev, ConnectionTerminated):
            self.closed.set()


async def run(host, port, paths):
    cfg = QuicConfiguration(is_client=True, alpn_protocols=["hq-interop"])
    cfg.verify_mode = ssl.CERT_NONE
    async with connect(host, port, configuration=cfg, create_protocol=HQ) as cli:
        await asyncio.wait_for(cli.hs.wait(), timeout=5)
        order = []
        for p in paths:
            sid = cli._quic.get_next_available_stream_id()
            cli._quic.send_stream_data(sid, ("GET %s\r\n" % p).encode(),
                                       end_stream=True)
            order.append(sid)
        cli.transmit()

        # Wait until every stream has seen its FIN (or the conn ends / timeout).
        deadline = len(paths)
        for _ in range(300):  # ~15s at 50ms granularity
            if len(cli.fins) >= deadline or cli.closed.is_set():
                break
            await asyncio.sleep(0.05)

        return [bytes(cli.bufs.get(sid, b"")) for sid in order]


async def main():
    host, port = sys.argv[1], int(sys.argv[2])
    rest = sys.argv[3:]

    if rest and rest[0] == "--mux":
        paths = rest[1:]
        bodies = await run(host, port, paths)
        out = []
        for p, b in zip(paths, bodies):
            out.append("%s %d %s" % (p, len(b), hashlib.sha1(b).hexdigest()))
        sys.stdout.write("\n".join(out) + "\n")
        return

    bodies = await run(host, port, rest)
    sys.stdout.buffer.write(bodies[0] if bodies else b"")


asyncio.run(main())
