#!/usr/bin/env python3
"""Drive the topic server with a WebSocket client we did not write.

The phpt suite proves topics against a hand-rolled RFC 6455 client living in the
same repo — which cannot catch a framing mistake made consistently on both sides.
The `websockets` library is an independent implementation and negotiates
permessage-deflate by default, so every publish below arrives as a compressed
frame decoded by code that has never seen ours.

Exits non-zero on the first failed check.

Usage:  python3 client.py [port]
"""

import asyncio
import sys

import websockets

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9101
URI = f"ws://127.0.0.1:{PORT}/"

failures = []


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}{f'  — {detail}' if detail else ''}")
    if not ok:
        failures.append(name)


async def subscribe(ws, topic_filter):
    await ws.send(f"sub {topic_filter}")
    ack = await asyncio.wait_for(ws.recv(), timeout=5)
    assert ack == f"ok {topic_filter}", ack


async def drain(ws, timeout=0.6):
    """Everything the peer has sent by now."""
    got = []
    while True:
        try:
            got.append(await asyncio.wait_for(ws.recv(), timeout=timeout))
        except asyncio.TimeoutError:
            return got


async def main():
    # Eight peers over four workers, so a subscriber is certainly on a worker
    # other than the publisher's — a same-worker delivery would prove nothing.
    peers = [await websockets.connect(URI) for _ in range(8)]
    pub, subs = peers[0], peers[1:]

    negotiated = [ext.name for ext in (pub.protocol.extensions or [])]
    check(
        "permessage-deflate negotiated with a third-party client",
        "permessage-deflate" in negotiated,
        ", ".join(negotiated) or "none",
    )

    await subscribe(pub, "ctl")  # so the publisher has an id to be excluded by

    for ws in subs:
        await subscribe(ws, "chat")
        await subscribe(ws, "user/+/msg")
        await subscribe(ws, "alerts/#")

    await asyncio.sleep(0.5)  # let every subscribe land in its worker's tree

    # 1. Plain topic, across workers.
    await pub.send("pub chat hello")
    got = [await drain(ws) for ws in subs]
    check(
        "publish reaches every worker's subscribers",
        all(g == ["hello"] for g in got),
        f"{sum(len(g) for g in got)}/{len(subs)} delivered",
    )
    check("publisher excluded from its own publish", await drain(pub) == [])

    # 2. '+' is exactly one level.
    await pub.send("pub user/42/msg plus")
    got = [await drain(ws) for ws in subs]
    check("'+' wildcard delivers", all(g == ["plus"] for g in got))

    await pub.send("pub user/42/deeper/msg nope")
    got = [await drain(ws) for ws in subs]
    check("'+' does NOT span several levels", all(g == [] for g in got))

    # 3. '#' takes the whole remainder.
    await pub.send("pub alerts/a/b/c hash")
    got = [await drain(ws) for ws in subs]
    check("'#' wildcard delivers at any depth", all(g == ["hash"] for g in got))

    # 4. A topic nobody subscribes to reaches nobody (and, behind the scenes,
    #    wakes no worker at all — that is the interest filter).
    await pub.send("pub void/topic x")
    got = [await drain(ws) for ws in subs]
    check("unsubscribed topic reaches nobody", all(g == [] for g in got))

    # 5. subscriberCount is a scatter/gather over the workers.
    await pub.send("count chat")
    reply = await asyncio.wait_for(pub.recv(), timeout=5)
    check(
        "subscriberCount tallies every worker",
        reply == f"count {len(subs)}",
        reply,
    )

    # 6. Binary survives the round trip through a topic.
    await pub.send("pub chat " + "x" * 4096)
    got = [await drain(ws, timeout=1.0) for ws in subs]
    check(
        "a 4 KiB payload arrives intact (compressed on the wire)",
        all(g == ["x" * 4096] for g in got),
    )

    for ws in peers:
        await ws.close()

    print()
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1

    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
